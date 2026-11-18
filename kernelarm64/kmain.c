/* ThreadOS on ARM64 - the kernel's first steps
 *
 * Stage one established, from the machine itself, the facts every
 * later stage stands on - the level it runs at, the processor, the
 * memory, the cores - and said them on the serial port, where a test
 * can hold them against what QEMU was told to build.
 *
 * Stage two made it a machine that can be relied on: exceptions are
 * caught and named instead of hanging the core, the MMU gives every
 * range of memory its proper kind and leaves address 0 unmapped, and
 * the interrupt controller delivers the timer, a hundred times a
 * second.
 *
 * Stage three gives it memory to hand out and more than one thing to
 * do at a time: pages from the device tree's RAM, the shared kernel
 * heap (kernel/heap.c, the same file the x86 kernels use) on top of
 * them, threads the timer takes turns between, and every core the
 * board has, woken through the firmware.
 *
 * Stage four is a phone's: a screen (QEMU's ramfb, upright, 720 x 1280),
 * the initrd with the fonts (through fw_cfg), the time (PL031), a
 * touchscreen (virtio-input) and on them ThreadOS mobile's first shell
 * (mobile.c), drawn by the PC's own graphics code - each only when the
 * machine has it, so a virt machine without them boots as before.
 *
 * Stage five runs programs: an AArch64 ELF file from the initrd in an
 * address space of its own, at EL0, entering the kernel only by a system
 * call, an interrupt or a fault - which ends the program, not the
 * kernel (proc.c). The command line's run=NAME,ARG,... words start
 * them, all at once, and their ends are said with their codes.
 *
 * Two self-tests can be asked for on the command line (QEMU's -append,
 * which lands in the device tree's /chosen/bootargs): selftest=brk
 * takes a breakpoint and must come back with the registers intact,
 * selftest=abort reads address 0x10 and must be stopped by the MMU.
 */

#include <stdint.h>
#include "arm64.h"
#include "uart.h"
#include "fdt.h"
#include "thread.h"
#include "fwcfg.h"
#include "video64.h"
#include "rtc.h"
#include "virtio.h"
#include "netglue.h"
#include "vsnd.h"
#include "../kernel/include/net.h"
#include "../kernel/include/dhcp.h"
#include "../kernel/include/tcp.h"
#include "../kernel/include/random.h"
#include "touch.h"
#include "mobile.h"
#include "proc.h"
#include "../kernel/include/string.h"
#include "../kernel/include/heap.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/initrd.h"
#include "../kernel/include/fonts.h"
#include "../kernel/include/video.h"
#include "../kernel/include/bootsplash.h"

/* Written by boot.S before it left EL2, if it was there */
extern uint32_t boot_el;
/* The end of the kernel's image and .bss, from the linker script */
extern char __kernel_end[];

/* Where QEMU's virt machine puts the device tree for a kernel that is
   not Linux: the very start of RAM */
#define VIRT_RAM_START 0x40000000UL
/* The most of it that is read: the gap the linker script leaves below
   the kernel. QEMU's tree is a megabyte at most. */
#define FDT_ROOM 0x200000u

/* MIDR_EL1 says who designed the core (bits 31..24) and which core it
   is (15..4). The names are Arm's; a core this list does not know is
   reported by its numbers. */
static const char *core_name(uint64_t midr)
{
    static const struct { uint16_t part; const char *name; } arm[] = {
        {0xD03, "Cortex-A53"}, {0xD04, "Cortex-A35"}, {0xD05, "Cortex-A55"},
        {0xD07, "Cortex-A57"}, {0xD08, "Cortex-A72"}, {0xD09, "Cortex-A73"},
        {0xD0A, "Cortex-A75"}, {0xD0B, "Cortex-A76"}, {0xD0C, "Neoverse N1"},
        {0xD0D, "Cortex-A77"}, {0xD40, "Neoverse V1"}, {0xD41, "Cortex-A78"},
        {0xD44, "Cortex-X1"},  {0xD46, "Cortex-A510"}, {0xD47, "Cortex-A710"},
        {0xD49, "Neoverse N2"},
    };
    if (((midr >> 24) & 0xFF) != 0x41)
        return 0;
    uint16_t part = (midr >> 4) & 0xFFF;
    for (unsigned i = 0; i < sizeof(arm) / sizeof(arm[0]); i++)
        if (arm[i].part == part)
            return arm[i].name;
    return 0;
}

/* Application packages (.tapp, scripts/mktapp.py): what the PC's loader
   hands over as modules, QEMU hands over as fw_cfg files under
   opt/threados/apps/ (-fw_cfg name=opt/threados/apps/weft,file=...).
   Each is checked and taken as on the PC (kernel/initrd.c) - an image of
   ThreadOS's, a manifest, programs for this processor - and its programs
   run like the initrd's. */
static void load_packages(void)
{
    uint16_t item;
    uint32_t size;
    for (int n = 0; fwcfg_find_nth("opt/threados/apps/", n, &item, &size) == 0;
         n++) {
        void *image = (void *)(uintptr_t)pmm_alloc_contiguous(
            (size + PAGE_SIZE - 1) / PAGE_SIZE);
        if (!image || fwcfg_read(item, image, size) != 0) {
            printk("[ !! ] a package could not be read (%u bytes)\n", size);
            continue;
        }
        int rc = package_use(image, size);
        if (rc < 0) {
            printk("[ !! ] a package passed over: %s\n", package_why(rc));
            continue;
        }
        const struct package *p = package_at(package_count() - 1);
        printk("[ OK ] Package %s %u: %s, %u KiB\n", p->id, p->version,
               p->name, size / 1024);
        mobile_notify(p->name, "Installed and ready to open");
    }
}

/* The value of a "name=value" word of the command line, into out */
static int word_value(const char *line, const char *name, char *out, int max)
{
    size_t n = strlen(name);
    for (const char *p = line; p && *p; ) {
        while (*p == ' ')
            p++;
        const char *start = p;
        while (*p && *p != ' ')
            p++;
        if ((size_t)(p - start) > n && !memcmp(start, name, n)) {
            int k = 0;
            for (const char *c = start + n; c < p && k < max - 1; c++)
                out[k++] = *c;
            out[k] = 0;
            return 1;
        }
    }
    return 0;
}

/* The PC's network stack (kernel/net.c, tcp.c, dhcp.c) on the virtio
   card, if there is one: DHCP asks for an address, and the programs on
   the command line start once it has one - or after a few seconds
   without, as on a machine with no network behind its card. "dns=IP"
   names the name server in place of DHCP's, as the PC's shell command
   'dns' does. */
static void start_network(struct virtio_dev *vdevs, int nv,
                          const char *line)
{
    if (vnet_init(vdevs, nv) < 0)
        return;
    dhcp_start();
    tcp_init();
    for (int i = 0; i < 800 && !net_configured(); i++)
        thread_sleep_ms(10);
    if (net_configured()) {
        uint32_t ip = net_ip();
        char s[48];
        int n = 0;
        const char *head = "Connected, ";
        while (head[n]) {
            s[n] = head[n];
            n++;
        }
        for (int i = 3; i >= 0; i--) {
            uint32_t v = (ip >> (i * 8)) & 0xFF;
            if (v >= 100)
                s[n++] = (char)('0' + v / 100);
            if (v >= 10)
                s[n++] = (char)('0' + v / 10 % 10);
            s[n++] = (char)('0' + v % 10);
            if (i)
                s[n++] = '.';
        }
        s[n] = 0;
        mobile_notify("Network", s);
    }
    char text[20];
    uint32_t ip;
    if (word_value(line, "dns=", text, sizeof(text)) &&
        net_parse_ip(text, &ip)) {
        net_enter();
        net_set_dns(ip);
        net_leave();
        printk("[net] DNS server set by the command line: %s\n", text);
    }
}

/* Every "run=NAME,ARG,ARG" word of the command line: the programs, all
   started before any is waited for - so that they run beside each other
   - and each one's end said with its code */
static void run_programs(const char *line)
{
    int pids[16];
    char names[16][16];
    int n = 0;
    uint32_t free_before = pmm_total_frames() - pmm_used_frames();
    for (const char *p = line; p && *p && n < 16; ) {
        while (*p == ' ')
            p++;
        const char *word = p;
        while (*p && *p != ' ')
            p++;
        if (p - word <= 4 || memcmp(word, "run=", 4))
            continue;
        char args[128];
        int k = 0;
        for (const char *c = word + 4; c < p && k < (int)sizeof(args) - 1;
             c++)
            args[k++] = *c == ',' ? ' ' : *c;
        args[k] = 0;
        int m = 0;
        while (args[m] && args[m] != ' ' && m < 15) {
            names[n][m] = args[m];
            m++;
        }
        names[n][m] = 0;
        pids[n] = proc_spawn(names[n], args);
        if (pids[n] < 0)
            printk("[proc] %s could not be started (%d)\n", names[n],
                   pids[n]);
        else
            printk("[proc] %s started, pid %d\n", names[n], pids[n]);
        n++;
    }
    for (int i = 0; i < n; i++)
        if (pids[i] > 0)
            printk("[proc] %s (pid %d) ended with code %d\n", names[i],
                   pids[i], proc_wait(pids[i]));
    /* And what they started and left behind */
    proc_wait_orphans();
    /* Every page a program had - its file, its stack, its heap and its
       tables - is back */
    if (n)
        printk("[proc] pages free: %u before the programs, %u after\n",
               free_before, pmm_total_frames() - pmm_used_frames());
}

/* Whether a word stands on its own in the command line */
static int has_word(const char *line, const char *word)
{
    if (!line)
        return 0;
    size_t n = strlen(word);
    const char *p = line;
    while (*p) {
        while (*p == ' ')
            p++;
        const char *start = p;
        while (*p && *p != ' ')
            p++;
        if ((size_t)(p - start) == n && memcmp(start, word, n) == 0)
            return 1;
    }
    return 0;
}

/* A value is put in x9 - a register the C handler is free to use for
   anything - and must still be there after the breakpoint: only the
   vector's saving and restoring can keep it. */
static void selftest_brk(void)
{
    uint64_t before = 0x0123456789ABCDEFULL, after;
    __asm__ volatile("mov x9, %1\n\t"
                     "brk #0x7a\n\t"
                     "mov %0, x9"
                     : "=r"(after) : "r"(before) : "x9", "memory");
    printk("Back from the breakpoint, x9 %s\n",
           after == before ? "kept" : "LOST");
}

/* Address 0x10 is in the flash on virt: readable with the MMU off,
   a translation fault with it on - unless the tables are wrong */
static void selftest_abort(void)
{
    volatile uint32_t *p = (volatile uint32_t *)0x10;
    uint32_t v = *p;
    printk("Read address 0x10 without a fault (0x%x)\n", v);
}

/* A 'ret' written into the last page of RAM, made visible to the
   instruction side, and called: outside the kernel's gigabyte RAM may
   not be run (mmu.c), so this has to end in an instruction abort. With
   one gigabyte there is no RAM outside the kernel's to try. */
static void selftest_xn(uint64_t ram_base, uint64_t ram_size)
{
    uint64_t last = ram_base + ram_size - 4096;
    if (last / (1UL << 30) == (uint64_t)(uintptr_t)selftest_xn / (1UL << 30)) {
        printk("No RAM outside the kernel's gigabyte to try running\n");
        return;
    }
    volatile uint32_t *code = (volatile uint32_t *)(uintptr_t)last;
    code[0] = 0xd65f03c0;                /* ret */
    __asm__ volatile("dc cvau, %0\n dsb ish\n ic ivau, %0\n dsb ish\n isb"
                     : : "r"(last) : "memory");
    printk("Running a data page at 0x%lx\n", last);
    ((void (*)(void))(uintptr_t)last)();
    printk("Ran code from a data page - nothing stopped it\n");
}

/* Blocks of many sizes, each filled with its own byte and read back
   whole before any is freed: two blocks handed out over each other
   would overwrite one another's byte. Ten rounds, freed in a different
   order from the one they were taken in, and at the end the heap must
   be exactly as full as it was. */
static void heap_selftest(void)
{
    enum { N = 200, ROUNDS = 10 };
    static uint8_t *blocks[N];
    static uint32_t sizes[N];
    uint32_t before = heap_used_bytes();
    unsigned bad = 0, missing = 0;

    for (unsigned round = 0; round < ROUNDS; round++) {
        for (unsigned i = 0; i < N; i++) {
            sizes[i] = 1 + (i * 37 + round * 101) % 2000;
            blocks[i] = kmalloc(sizes[i]);
            if (!blocks[i]) {
                missing++;
                continue;
            }
            memset(blocks[i], (int)(i ^ round), sizes[i]);
        }
        for (unsigned i = 0; i < N; i++)
            for (uint32_t k = 0; blocks[i] && k < sizes[i]; k++)
                if (blocks[i][k] != (uint8_t)(i ^ round)) {
                    bad++;
                    break;
                }
        for (unsigned i = 1; i < N; i += 2)
            kfree(blocks[i]);
        for (unsigned i = 0; i < N; i += 2)
            kfree(blocks[i]);
    }
    printk("Heap: %u allocations, %u refused, %u overlapping, %u bytes "
           "still in use after freeing them all\n", N * ROUNDS, missing,
           bad, heap_used_bytes() - before);
    /* Sizes the arithmetic could wrap on - near the top of size_t,
       rounding up to 8 makes 0 - and one just past the whole heap:
       none of them can be had (kernel/heap.c) */
    unsigned given = 0;
    if (kmalloc((size_t)-3))
        given++;
    if (kmalloc((size_t)-64))
        given++;
    if (kmalloc((size_t)heap_total_bytes() + 1))
        given++;
    printk("Heap: impossible sizes given %u times of 3\n", given);
}

/* Threads that never give the processor up of their own accord: only
   the timer can take it from them. Each counts as fast as it can; how
   far each got is its share of the time. */
#define SPINNERS 4
static volatile uint64_t spins[SPINNERS];
static volatile int stop;

static void spinner(void *arg)
{
    unsigned i = (unsigned)(uintptr_t)arg;
    while (!stop)
        spins[i]++;
}

static void threads_selftest(void)
{
    uint32_t heap_before = heap_used_bytes();
    struct thread *t[SPINNERS];
    for (unsigned i = 0; i < SPINNERS; i++)
        t[i] = thread_create("spinner", spinner, (void *)(uintptr_t)i);
    printk("Threads: %d that never yield, preempted %d times a second\n",
           SPINNERS, HZ);

    uint64_t start = counter_now();
    thread_sleep_ms(500);
    uint64_t slept = (counter_now() - start) * 1000 / timer_freq();
    printk("Threads: main slept 500 ms and woke after %lu ms by the "
           "counter\n", slept);

    stop = 1;
    uint32_t cores = 0;
    uint64_t total = 0;
    for (unsigned i = 0; i < SPINNERS; i++) {
        cores |= t[i]->cores;
        thread_join(t[i]);
        total += spins[i];
    }
    printk("Threads: shares");
    for (unsigned i = 0; i < SPINNERS; i++)
        printk(" %lu%%", total ? spins[i] * 100 / total : 0);
    printk("\n");
    printk("Threads ran on cores:");
    for (unsigned c = 0; c < MAX_CORES; c++)
        if (cores & (1u << c))
            printk(" %u", c);
    printk("\n");
    printk("Threads: all %d joined, %u bytes of the heap not given back\n",
           SPINNERS, heap_used_bytes() - heap_before);
}

void kmain(uint64_t x0)
{
    /* First of all, so that whatever goes wrong next is said in words */
    exceptions_init();

    printk("\nThreadOS on ARM64\n");
    printk("Entered at EL%u, running at EL%u\n", boot_el,
           (uint32_t)(read_sysreg(CurrentEL) >> 2) & 3);

    uint64_t midr = read_sysreg(midr_el1);
    const char *name = core_name(midr);
    if (name)
        printk("CPU: Arm %s (MIDR %x)\n", name, (uint32_t)midr);
    else
        printk("CPU: implementer %x, part %x (MIDR %x)\n",
               (uint32_t)(midr >> 24) & 0xFF, (uint32_t)(midr >> 4) & 0xFFF,
               (uint32_t)midr);

    /* The start of RAM first, because that is where QEMU documents it
       for this kind of kernel; x0 only after that, because x0 might be
       anything, and an address with no memory behind it is an abort. */
    const void *dtb = 0;
    const char *where = "";
    if (fdt_valid((const void *)VIRT_RAM_START)) {
        dtb = (const void *)VIRT_RAM_START;
        where = "the start of RAM";
    } else if (x0 && (x0 & 7) == 0 && x0 >= VIRT_RAM_START &&
               fdt_valid((const void *)x0)) {
        dtb = (const void *)x0;
        where = "x0";
    }
    if (!dtb) {
        printk("No device tree - nothing more can be known about this "
               "machine.\n");
        arm64_halt();
    }
    printk("Device tree: %u bytes at 0x%lx (found at %s)\n",
           fdt_size(dtb), (uint64_t)(uintptr_t)dtb, where);

    static struct fdt_info fi;
    if (fdt_scan(dtb, FDT_ROOM, &fi) < 0) {
        printk("The device tree does not add up - stopping here.\n");
        arm64_halt();
    }
    printk("Memory: %lu MiB at 0x%lx\n", fi.mem_total >> 20, fi.mem_base);
    printk("Cores: %d\n", fi.cpus);
    if (fi.bootargs && fi.bootargs[0])
        printk("Command line: %s\n", fi.bootargs);

    if (mmu_build(fi.mem_base, fi.mem_total) < 0) {
        printk("Memory this kernel cannot map yet - stopping here.\n");
        arm64_halt();
    }
    mmu_switch_on();
    uart_lock_output();
    uint64_t sctlr = read_sysreg(sctlr_el1);
    printk("MMU on: RAM cached, devices not, address 0 unmapped "
           "(SCTLR M%u C%u I%u)\n", (uint32_t)(sctlr & 1),
           (uint32_t)(sctlr >> 2) & 1, (uint32_t)(sctlr >> 12) & 1);

    pmm_init_range(fi.mem_base, fi.mem_total,
                   (uint64_t)(uintptr_t)__kernel_end);
    printk("Pages: %u free of %u, %u under the device tree and the "
           "kernel\n", pmm_total_frames() - pmm_used_frames(),
           pmm_total_frames(), pmm_used_frames());
    heap_init();
    printk("Heap: %u KiB\n", heap_total_bytes() / 1024);
    heap_selftest();

    /* --- a phone's devices, where the machine has them ------------- */
    static struct mobile_info mi;
    static struct virtio_dev vdevs[FDT_MAX_VIRTIO];
    int screen = 0;
    if (fwcfg_init(fi.fwcfg) == 0) {
        /* The screen first, so that the loading screen - the PC's own,
           kernel/bootsplash.c - is up for the rest of the start */
        /* panel=720x1600: a phone's own panel (the Moto G34's), where
           the tests' phone is 720x1280 */
        int pw = 720, ph = 1280;
        char v[16];
        if (word_value(fi.bootargs, "panel=", v, sizeof(v))) {
            int a = 0, b = 0, i = 0;
            while (v[i] >= '0' && v[i] <= '9')
                a = a * 10 + (v[i++] - '0');
            if (v[i] == 'x')
                for (i++; v[i] >= '0' && v[i] <= '9'; i++)
                    b = b * 10 + (v[i] - '0');
            if (a >= 320 && a <= 2048 && b >= 480 && b <= 4096) {
                pw = a;
                ph = b;
            }
        }
        int r = video_ramfb_init(pw, ph, 32);
        if (r == 0) {
            screen = 1;
            printk("Screen: ramfb, %dx%d, two pixels to one\n", pw, ph);
            boot_splash_start(1);
        } else if (r != -1) {
            printk("Screen: ramfb would not start (%d)\n", r);
        }
        uint16_t item;
        uint32_t size;
        if (fwcfg_find("opt/threados/initrd", &item, &size) == 0) {
            void *image = (void *)(uintptr_t)pmm_alloc_contiguous(
                (size + PAGE_SIZE - 1) / PAGE_SIZE);
            if (image && fwcfg_read(item, image, size) == 0 &&
                initrd_use(image, size) == 0) {
                printk("initrd: %u files, %u bytes, through fw_cfg\n",
                       initrd_file_count(), size);
                fonts_init();
            } else {
                printk("initrd: fw_cfg has one, but it could not be "
                       "read or is not an initrd\n");
            }
        }
        load_packages();
        boot_splash_update(25, "Loading system files");
    }
    if (fi.rtc) {
        rtc_init(fi.rtc);
        struct rtc_time t;
        rtc_split(rtc_seconds(), &t);
        printk("Clock: %u-%u-%u %u:%u:%u UTC\n", (uint32_t)t.year,
               (uint32_t)t.month, (uint32_t)t.day, (uint32_t)t.hour,
               (uint32_t)t.min, (uint32_t)t.sec);
    }
    int nv = virtio_probe(fi.virtio_base, fi.virtio_irq, fi.virtio_n, vdevs,
                          FDT_MAX_VIRTIO);
    /* The PC's pool of random numbers, and the entropy device's bytes in
       it when the machine has one */
    random_init();
    vrng_init(vdevs, nv);
    vsnd_init(vdevs, nv);
    if (screen && touch_init(vdevs, nv, video_w(), video_h()) == 0)
        mi.touch = touch_name();
    static char cpu_text[32];
    if (name) {
        char *p = cpu_text;
        const char *a = "Arm ";
        while (*a)
            *p++ = *a++;
        for (const char *n = name; *n && p < cpu_text + sizeof(cpu_text) - 1;)
            *p++ = *n++;
        *p = 0;
    }
    mi.cpu = name ? cpu_text : "an ARM64 core";
    mi.cores = fi.cpus;
    {
        /* scale=150 or 175: the phone's display size from the start */
        char v[8];
        if (word_value(fi.bootargs, "scale=", v, sizeof(v))) {
            int n = 0;
            for (int i = 0; v[i] >= '0' && v[i] <= '9'; i++)
                n = n * 10 + (v[i] - '0');
            mi.scale = n;
        }
    }
    mi.mem_mib = (uint32_t)(fi.mem_total >> 20);
    mi.screen_w = video_pw();
    mi.screen_h = video_ph();

    if (has_word(fi.bootargs, "selftest=brk"))
        selftest_brk();
    if (has_word(fi.bootargs, "selftest=abort"))
        selftest_abort();
    if (has_word(fi.bootargs, "selftest=xn"))
        selftest_xn(fi.mem_base, fi.mem_total);

    sched_init();
    if (gic_init(fi.gic_version, fi.gic_dist, fi.gic_cpu) < 0) {
        if (fi.gic_version == 3)
            printk("Interrupt controller: GICv3 at 0x%lx - not supported "
                   "yet, so no timer.\n", fi.gic_dist);
        else
            printk("No interrupt controller this kernel knows - so no "
                   "timer.\n");
        printk("No timer, so no threads and no other cores.\n");
    } else {
        printk("Interrupt controller: GICv2, distributor 0x%lx, "
               "CPU interface 0x%lx\n", fi.gic_dist, fi.gic_cpu);
        printk("Timer: a %lu Hz counter, %u ticks a second\n",
               timer_freq(), HZ);
        uint64_t start = counter_now();
        timer_start(HZ);
        irq_enable();
        while (timer_ticks() < HZ)
            __asm__ volatile("wfi");
        uint64_t ms = (counter_now() - start) * 1000 / timer_freq();
        printk("Timer: %u ticks took %lu ms by the counter\n", HZ, ms);
        boot_splash_update(50, "Waking the cores");

        smp_start(&fi);
        boot_splash_update(70, "Checking the threads");
        threads_selftest();
        start_network(vdevs, nv, fi.bootargs);
        /* The phone before the programs: a program's window needs a
           screen to go to */
        boot_splash_update(96, "Starting the phone");
        if (screen && mobile_init(&mi) == 0)
            thread_create("mobile", mobile_main, 0);
        run_programs(fi.bootargs);
    }

    printk("The ARM64 kernel is ready.\n");
    for (;;)
        __asm__ volatile("wfi");          /* the timer keeps ticking */
}
