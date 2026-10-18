/* ThreadOS 64 - the first step of the port to long mode
 *
 * By this point the processor has come all the way through
 * boot64/boot64.S: long mode is on, it has its own descriptor table
 * and the first four gigabytes mapped one to one with 2 MiB pages.
 *
 * This file is still a skeleton. What matters is what is NOT here:
 * not a single copy of code from the 32-bit kernel. Printing, the
 * framebuffer console, reading Multiboot 2 tags and string handling
 * are the same files, compiled a second time - the differences
 * between the modes fit inside the io.h header and a few casts.
 */

#include "../kernel/include/fonts.h"
#include <stdint.h>
#include "../kernel/include/mb2.h"
#include "../kernel/include/fbcon.h"
#include "../kernel/include/video.h"
#include "../kernel/include/mouse.h"
#include "../kernel/include/vga.h"
#include "../kernel/include/serial.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/pic.h"
#include "../kernel/include/pit.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/heap.h"
#include "idt64.h"
#include "paging64.h"
#include "../kernel/include/tlb.h"
#include "../kernel/include/config.h"
#include "task64.h"
#include "gdt64.h"
#include "syscall64.h"
#include "elf64.h"
#include "proc64.h"
#include "keyboard64.h"
#include "shell64.h"
#include "../kernel/include/acpi.h"
#include "../kernel/include/usbkbd.h"
#include "../kernel/include/usbmouse.h"
#include "../kernel/include/bt.h"
#include "../kernel/include/dynlink.h"
#include "../kernel/include/biglock.h"
#include "../kernel/include/swap.h"
#include "fpu64.h"
#include "isr64.h"
#include "../kernel/include/initrd.h"
#include "../kernel/include/blk.h"
#include "../kernel/include/hda.h"
#include "../kernel/include/fs.h"
#include "../kernel/include/net.h"
#include "../kernel/include/netdev.h"
#include "../kernel/include/smp.h"
#include "../kernel/include/dhcp.h"
#include "../kernel/include/tcp.h"
#include "../kernel/include/string.h"
#include "../kernel/include/desktop.h"
#include "../kernel/include/install.h"

/* Demonstration threads: two counting forever and one that ends by
   itself - so that a zombie can be seen disappearing from the list. */
static volatile uint32_t counter_a, counter_b;
static volatile int mortal_done;

static void count_a(void *arg)
{
    (void)arg;
    for (;;) {
        counter_a++;
        task64_sleep(20);
    }
}

static void count_b(void *arg)
{
    (void)arg;
    for (;;) {
        counter_b++;
        task64_sleep(50);
    }
}

static void mortal(void *arg)
{
    (void)arg;
    task64_sleep(300);
    mortal_done = 1;
}

/* A thread that drops down to ring 3.
 *
 * The program code lies in the kernel image, but ring 3 has no right
 * to read it there - so we copy it into a fresh frame and map that
 * frame as user-accessible. The program stack is a second such frame.
 *
 * The kernel stack works differently than with an interrupt gate: the
 * syscall instruction does not switch stacks, so the processor has to
 * be told the address of our own stack up front - and twice, because
 * an interrupt from ring 3 takes it from somewhere else (from the TSS)
 * than a system call does. */
extern const uint8_t user_payload_start[], user_payload_end[];

#define USER_CODE_VA  0x0000000040000000ULL
#define USER_STACK_VA 0x0000000040001000ULL

static void ring3_thread(void *arg)
{
    (void)arg;

    uint32_t code = pmm_alloc_frame();
    uint32_t stack = pmm_alloc_frame();
    if (!code || !stack) {
        printk("[ !! ] Ring 3: no memory\n");
        task64_exit();
    }

    uint64_t len = (uint64_t)(user_payload_end - user_payload_start);
    memcpy((void *)(uintptr_t)code, user_payload_start, (uint32_t)len);

    /* Code with no write permission, stack with no execute permission -
       ring 3 sees both, but neither is written and executed at once */
    if (paging64_map(USER_CODE_VA, code, PG_USER) < 0 ||
        paging64_map(USER_STACK_VA, stack, PG_USER | PG_WRITE | PG_NX) < 0) {
        printk("[ !! ] Ring 3: the program cannot be mapped\n");
        task64_exit();
    }

    /* Top of our own stack - the kernel starts here on every entry
       from ring 3 */
    uint64_t kstack;
    __asm__ volatile("movq %%rsp, %0" : "=r"(kstack));
    task64_enter_space(0, kstack & ~0xFULL);   /* kernel space */

    enter_ring3_64(USER_CODE_VA, USER_STACK_VA + PAGE_SIZE - 16, 0, 0);
}

/* A thread that runs a PROGRAM FROM A FILE.
 *
 * The difference from the thread above is fundamental, even though it
 * ends with the same jump: there the code lay in the kernel image,
 * here the kernel is handed a file it never saw at build time, and
 * everything - where to start, what to load, with what permissions -
 * it learns from the ELF header. This is the line beyond which the
 * system starts running somebody else's programs rather than pieces
 * of itself. */
/* Runs a program and waits for it to finish - on its own or killed by
   the kernel. Returns 1 if it started at all. */
static int run_program(const char *name)
{
    syscall64_forget_user();
    isr64_forget_user_fault();

    if (proc64_spawn(name, 0, 0) < 0)
        return 0;
    for (int i = 0; i < 80; i++) {
        if (syscall64_user_exited() || isr64_user_fault())
            break;
        task64_sleep(50);
    }
    return 1;
}

/* Is the processor really in long mode? It asks itself: the LMA bit in
   the EFER register is set by the processor, not by us. */
static int long_mode_active(void)
{
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080));
    (void)high;
    return (low & (1u << 10)) != 0;
}

/* Is this word on the boot command line? GRUB passes the line with the
   kernel's entry in grub.cfg; whole words only, so 'desktop' does not
   match 'nodesktop'. */
static int boot_word(const struct multiboot_info *mbi, const char *word)
{
    if (!mbi || !(mbi->flags & MULTIBOOT_FLAG_CMDLINE) || !mbi->cmdline)
        return 0;
    const char *cmd = (const char *)(uintptr_t)mbi->cmdline;
    size_t n = strlen(word);
    for (int i = 0; cmd[i]; i++) {
        if (i && cmd[i - 1] != ' ')
            continue;
        size_t k = 0;
        while (k < n && cmd[i + k] == word[k])
            k++;
        if (k == n && (cmd[i + k] == '\0' || cmd[i + k] == ' '))
            return 1;
    }
    return 0;
}

void kmain64(uint32_t magic, uint32_t info_addr)
{
    const struct multiboot_info *mbi = mb2_info(magic, info_addr);

    const struct mb2_framebuffer *lfb = mb2_framebuffer();
    /* The loader's buffer is taken over by video: it sets up both the
       console (through fbcon) and its own drawing state. That way text
       mode and graphics mode are the same buffer, only used
       differently. */
    int fb_taken = lfb && video_adopt_lfb(lfb->addr, lfb->pitch,
                                          (int)lfb->width,
                                          (int)lfb->height,
                                          (int)lfb->bpp) == 0;
    if (fb_taken) {
        video_panel_mm((int)lfb->mm_w, (int)lfb->mm_h);
        console_use_framebuffer();
    }

    vga_init();
    serial_init();

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    printk("ThreadOS 64\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    printk("The kernel is running in long mode (x86-64).\n\n");

    if (!mbi) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        printk("ERROR: unknown magic from the boot loader (%x)!\n", magic);
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    printk("[ OK ] Multiboot OK (%s)\n", mb2_boot_kind());

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    printk("[ OK ] ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    printk("Long mode: LMA=%u, a pointer is %u bytes\n",
           (uint32_t)long_mode_active(), (uint32_t)sizeof(void *));

    if (mbi->flags & MULTIBOOT_FLAG_MODS)
        printk("[ OK ] Modules from the loader: %u\n", mbi->mods_count);

    if (fbcon_have())
        printk("[ OK ] Framebuffer: %ux%u, %u bits per pixel\n",
               (uint32_t)fbcon_width(), (uint32_t)fbcon_height(),
               (uint32_t)fbcon_bpp());
    else if (lfb)
        printk("[ !! ] The %ux%u framebuffer has %u bits per pixel - "
               "we can read 24 and 32\n", lfb->width, lfb->height, lfb->bpp);
    else
        printk("[ !! ] The loader gave no framebuffer - the serial port only\n");

    /* Under UEFI there is no BIOS memory to search - the pointer to the
       ACPI tables is then supplied by the loader alone. The tables
       themselves are read later: they lie high up and first have to be
       added to the mapping, and there is no paging here yet. */
    if (mb2_rsdp()) {
        printk("[ OK ] ACPI pointer from the loader: %x\n", mb2_rsdp());
        acpi_set_rsdp(mb2_rsdp());
    }

    /* --- Memory ------------------------------------------------- */
    pmm_init(mbi);
    printk("[ OK ] Physical memory: %u KiB RAM, %u free 4 KiB frames\n",
           pmm_ram_kib(), pmm_total_frames() - pmm_used_frames());

    /* The framebuffer lies high up, outside the memory described at
       startup - it has to be added to the mapping before we write
       anything to it */
    if (fbcon_have())
        video_map_lfb();

    if (paging64_init() < 0) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        printk("ERROR: the page tables could not be built\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    /* From now on every change of a mapping can be told to the other
       cores - and to do that they have to know what each is running */
    tlb_switch(paging64_kernel_space());
    printk("[ OK ] Four-level paging: %u MiB, execute-disable "
           "%s, screen memory %s\n",
           (uint32_t)(paging64_identity_top() / (1024 * 1024)),
           paging64_nx_enabled() ? "on" : "off",
           paging64_fb_wc() ? "write-combining" : "uncached");

    /* Fine-grained mapping: a frame placed high up, far beyond the area
       described one to one. Writing and reading through that address
       proves the whole four-level path works. */
    uint32_t frame = pmm_alloc_frame();
    uint64_t high = 0x0000000200000000ULL;      /* eight gigabytes */
    if (frame && paging64_map(high, frame, PG_WRITE | PG_NX) == 0) {
        volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)high;
        *p = 0x0123456789ABCDEFULL;
        uint64_t phys = paging64_lookup(high);
        printk("[ OK ] A 4 KiB mapping at 8 GiB: read %x%x, "
               "physical address %x\n",
               (uint32_t)(*p >> 32), (uint32_t)*p, (uint32_t)phys);
    } else {
        printk("[ !! ] The 4 KiB mapping did not work\n");
    }

    heap_init();
    /* A quick heap check: allocate, write, free and see whether the
       memory returns to the pool - the same test as in the 32-bit
       kernel */
    char *a = kmalloc(1024);
    char *b = kmalloc(4096);
    int heap_ok = a && b && a != b;
    if (heap_ok) {
        for (int i = 0; i < 1024; i++)
            a[i] = (char)i;
        for (int i = 0; i < 1024; i++)
            if (a[i] != (char)i)
                heap_ok = 0;
    }
    uint32_t used = heap_used_bytes();
    kfree(a);
    kfree(b);
    printk("[ OK ] Kernel heap: %s (peak %u B, after freeing %u B)\n",
           heap_ok ? "kmalloc/kfree work" : "ERROR", used,
           heap_used_bytes());

    /* --- Interrupts --------------------------------------------- */
    idt64_init();
    printk("[ OK ] IDT loaded (exceptions 0-31, IRQ 32-47)\n");

    pic_init();
    printk("[ OK ] PIC remapped (IRQ -> vectors 32-47)\n");

    pit_init(100);
    printk("[ OK ] Timer PIT: 100 Hz\n");

    __asm__ volatile("sti");
    printk("[ OK ] Interrupts enabled (sti)\n");

    /* A trap exception: it exercises the whole path - stub, tables,
       register frame and the return through iretq - without stopping
       the system */
    __asm__ volatile("int $3");

    /* The timer has to tick on its own: we wait for interrupts and watch
       whether the counter moves. That is the proof that hardware
       interrupts arrive. */
    uint32_t start = pit_ticks();
    for (int i = 0; i < 200; i++)
        __asm__ volatile("hlt");
    printk("[ OK ] The timer ticks: %u -> %u ticks\n", start, pit_ticks());

    /* --- Threads ------------------------------------------------ */
    /* SSE has to be enabled BEFORE the scheduler: the first task gets
       room for the register state right away, and its pattern comes
       from a freshly initialized unit */
    if (fpu64_init())
        printk("[ OK ] The floating point unit and SSE are on\n");

    scheduler64_init();
    task64_create("count-A", count_a, 0);
    task64_create("count-B", count_b, 0);
    task64_create("short", mortal, 0);
    printk("[ OK ] Scheduler: %d tasks on the list\n", task64_count());

    /* The main thread goes to sleep; the processor has to move on to the
       others meanwhile. If context switching were broken, the counters
       would stay at zero or the system would hang. */
    task64_sleep(600);
    printk("[ OK ] Threads counting side by side: A=%u, B=%u\n",
           counter_a, counter_b);
    printk("[ OK ] A thread that ends: %s, tasks on the list: %d\n",
           mortal_done ? "did its work" : "did not make it", task64_count());

    /* --- Ring 3 -------------------------------------------------- */
    gdt64_init();
    printk("[ OK ] A GDT with ring 3 segments and a TSS\n");

    syscall64_init();
    printk("[ OK ] System calls through the syscall instruction\n");

    task64_create("ring3", ring3_thread, 0);
    task64_sleep(200);

    /* Only the processor knows the code selector - the two lowest bits
       say which ring the program really ran in */
    uint32_t cs = syscall64_user_cs();
    printk("[ OK ] A program in ring 3: %s, cs=%x, ring %u\n",
           syscall64_user_ran() ? "answered" : "SILENT", cs, cs & 3);

    /* --- Programs from the initrd ------------------------------- */
    initrd_init(mbi);
    fonts_init();
    if (!initrd_file_count())
        printk("[ !! ] initrd: no programs - there is nothing to run\n");
    else
        printk("[ OK ] initrd: %u file(s) from the loader\n",
               initrd_file_count());

    /* The shared library has to be ready before the first program
       starts - its frames are common to all processes */
    if (dyn_init())
        printk("[ OK ] Library %s: %u pages shared between processes\n",
               dyn_name(), dyn_pages());

    if (initrd_file_count()) {
    /* A program from a file gets a clean slate: whatever it tells the
       kernel, it says on its own, not as a piece of code built into
       the kernel image */
    if (run_program("hello64")) {
        cs = syscall64_user_cs();
        printk("[ OK ] ELF64 from the initrd: ring %u, exit code %u\n",
               cs & 3, (uint32_t)syscall64_user_status());
    }

    /* --- Memory protection as seen from the program -------------- */
    /* The no-execute bit had only been set so far. Now a program tries
       to execute its own data, and page faults from ring 3 kill the
       guilty one instead of stopping the system - so this can be
       checked and survived. */
    if (run_program("nx64"))
        printk("[ %s ] Execute-disable: %s (err=%x, address=%x)\n",
               isr64_user_fault() ? "OK" : "!!",
               isr64_user_fault() ? "the processor refused to execute data"
                                  : "THE PROGRAM EXECUTED ITS OWN DATA",
               isr64_user_fault_err(), (uint32_t)isr64_user_fault_addr());

    if (run_program("wild64"))
        printk("[ %s ] Kernel memory: %s (err=%x, address=%x)\n",
               isr64_user_fault() ? "OK" : "!!",
               isr64_user_fault() ? "out of reach of ring 3"
                                  : "THE PROGRAM READ IT",
               isr64_user_fault_err(), (uint32_t)isr64_user_fault_addr());

    /* The most important proof: after those two deaths the system keeps
       running */
    uint32_t a_before = counter_a;
    task64_sleep(200);
    printk("[ %s ] The system survived the programs dying (counter %u -> %u)\n",
           counter_a > a_before ? "OK" : "!!", a_before, counter_a);

    /* --- Two processes at once, each in its own space ------------ */
    /* The same copy of the same program, both at the same virtual
       address - and each with its own memory. The only thing that
       tells them apart is the number they were given in RDI. */
    uint32_t frames_before = pmm_used_frames();
    int base_tasks = task64_count();
    syscall64_forget_user();

    /* The same copy of the program, and one thing tells them apart: the
       argument. The process number now arrives as argv[1] rather than
       written into a register - that is, the way every program gets
       it. */
    static struct args64 sep_a = { 2, { "sep64", "1" } };
    static struct args64 sep_b = { 2, { "sep64", "2" } };
    proc64_spawn("sep64", &sep_a, 1);
    proc64_spawn("sep64", &sep_b, 1);
    for (int i = 0; i < 120 && task64_count() > base_tasks; i++)
        task64_sleep(50);

    int exits = syscall64_user_exits();
    int fails = syscall64_user_failures();
    printk("[ %s ] Two processes at once: %u finished, %u errors\n",
           (exits == 2 && fails == 0) ? "OK" : "!!",
           (uint32_t)exits, (uint32_t)fails);

    /* The spaces have to disappear along with the processes - otherwise
       every program that ran would take memory away forever */
    uint32_t frames_after = pmm_used_frames();
    printk("[ %s ] The processes' memory came back (%u -> %u frames)\n",
           frames_after <= frames_before ? "OK" : "!!",
           frames_before, frames_after);

    /* --- A process that duplicates itself ----------------------- */
    /* Here the kernel does nothing beyond handing over the program: the
       second process comes from a fork, that is, from the program's
       own decision. */
    frames_before = pmm_used_frames();
    base_tasks = task64_count();
    syscall64_forget_user();

    proc64_spawn("fork64", 0, 1);
    for (int i = 0; i < 160 && task64_count() > base_tasks; i++)
        task64_sleep(50);

    exits = syscall64_user_exits();
    fails = syscall64_user_failures();
    printk("[ %s ] A process copied itself: %u finished, %u errors\n",
           (exits == 2 && fails == 0) ? "OK" : "!!",
           (uint32_t)exits, (uint32_t)fails);
    frames_after = pmm_used_frames();
    printk("[ %s ] The fork's memory came back (%u -> %u frames)\n",
           frames_after <= frames_before ? "OK" : "!!",
           frames_before, frames_after);

    /* --- fork + exec + wait ------------------------------------- */
    /* From here it is only a step to a shell: a program can run ANOTHER
       program and wait for it to end. */
    base_tasks = task64_count();
    syscall64_forget_user();

    proc64_spawn("spawn64", 0, 1);
    for (int i = 0; i < 200 && task64_count() > base_tasks; i++)
        task64_sleep(50);

    /* We are not counting non-zero exits here: hello64 ends with a seven
       ON PURPOSE, and that seven is exactly the proof that the parent
       waited for the right child. The verdict is passed by spawn64
       itself - its exit code is the last one. */
    exits = syscall64_user_exits();
    int verdict = syscall64_user_status();
    printk("[ %s ] fork + exec + wait: %u finished, verdict %u\n",
           (exits == 2 && verdict == 0) ? "OK" : "!!",
           (uint32_t)exits, (uint32_t)verdict);

    /* --- Floating-point numbers in two processes at once -------- */
    /* If the SSE registers were not saved on a task switch, each of
       these processes would get the other one's result */
    base_tasks = task64_count();
    syscall64_forget_user();

    static struct args64 fpu_a = { 2, { "fpu64test", "3" } };
    static struct args64 fpu_b = { 2, { "fpu64test", "7" } };
    proc64_spawn("fpu64test", &fpu_a, 1);
    proc64_spawn("fpu64test", &fpu_b, 1);
    for (int i = 0; i < 160 && task64_count() > base_tasks; i++)
        task64_sleep(50);

    exits = syscall64_user_exits();
    fails = syscall64_user_failures();
    printk("[ %s ] The SSE state survived switching: %u counted, "
           "%u mistakes\n",
           (exits == 2 && fails == 0) ? "OK" : "!!",
           (uint32_t)exits, (uint32_t)fails);

    /* fork no longer copies memory - it shares it and separates only
       those pages somebody actually wrote to */
    uint32_t splits = paging64_cow_splits();
    uint32_t cost = proc64_last_fork_cost();
    printk("[ %s ] Copy on write: the fork took %u frames, "
           "%u pages were split\n",
           (splits > 0 && splits < 16 && cost < 8) ? "OK" : "!!",
           cost, splits);
    }

    task64_dump();

    /* --- Power -------------------------------------------------- */
    /* acpi.c came across like the rest of the shared code: the path to
       powering off - RSDP, the table list, the FADT, the _S5_ object
       in bytecode - does not depend on the processor mode. The only
       thing that does is adding tables to the mapping, and that is all
       this file shows about the mode. */
    if (acpi_init())
        printk("[ OK ] ACPI: tables from '%s' (the 'poweroff', "
               "'reboot')\n", acpi_oem());
    else
        printk("[ !! ] ACPI: no tables - we cannot switch the machine off\n");

    /* --- Disk --------------------------------------------------- */
    /* PCI, ATA, AHCI and the block layer are the same files as in the
       32-bit kernel. The drivers know nothing about the processor
       mode: they talk to the device through ports and memory, and both
       look the same on either side. */
    if (blk_init()) {
        /* The file system is the same file as in the 32-bit kernel too -
           and here not a single character had to change in it. The disk
           seen through the block layer looks the same regardless of the
           processor mode. */
        int fs = fs_init();
        printk("[ OK ] Disk %s + TFS %s (%u sectors)\n",
               blk_name(), fs ? "ready" : "OUT OF REACH", blk_sectors());

        /* What the machine remembers about itself. Right after the file
           system, because that is where it is written. */
        config_load();
        printk("[ OK ] Settings: %s (%d entries, the 'set' command)\n",
               config_get("name"), config_count());
        /* Page swapping: the space is reserved through the file system, but
           the pages themselves travel there straight through the block
           layer. It has to come after fs_init, because without a file
           system there is nowhere to put them. */
        if (fs)
            swap_init();
    } else {
        printk("[ !! ] Disk: no controller answered\n");
    }

    /* --- Network ------------------------------------------------ */
    /* The card, the IP layer, DHCP and TCP are again the same files as
       in the 32-bit kernel. Three of the four came across without a
       single change - the card driver only needed explicit casts of
       addresses, because the card addresses memory with 32 bits while
       a pointer here is eight bytes wide. */
    int net = net_init();
    if (net) {
        const uint8_t *m = netdev_mac();
        printk("[ OK ] Network: %s %x:%x:%x:%x:%x:%x\n", netdev_name(),
               m[0], m[1], m[2], m[3], m[4], m[5]);
    } else {
        printk("[ !! ] Network: I found no network card\n");
    }

    /* --- The remaining processors ------------------------------- */
    /* The MADT table lists them all, and each one waits for an
       INIT-SIPI signal. A secondary core starts in real mode - and has
       to walk the whole road to long mode itself, because nobody here
       will help it. It takes the page tables ready-made, the same ones
       the kernel walks. Once woken it reports in and stands by: giving
       it work needs a per-core scheduler and locks, and there are none
       of those yet. */
    int ncores = smp_init();
    if (ncores > 1)
        printk("[ OK ] Processors: %d (woken %d, waiting for work)\n",
               ncores, smp_started());
    else
        printk("[ OK ] Processors: 1\n");

    /* --- Keyboard input ----------------------------------------- */
    keyboard64_init();
    if (mouse_init())
        printk("[ OK ] PS/2 keyboard (IRQ 1) and mouse (IRQ 12), wheel: %s\n",
               mouse_has_wheel() ? "yes" : "no");
    else
        printk("[ OK ] PS/2 keyboard (IRQ 1); no PS/2 mouse\n");

    /* Machines newer than roughly 2010 no longer have a PS/2 connector.
       The USB driver and the HID class are again the same files as in
       the 32-bit kernel: the controller structures have to lie at an
       address the controller itself understands, and in both modes
       that means an identity mapping. */
    usbkbd_init();          /* it reports in by itself when it finds something */
    usbmouse_init();
    /* And the adapter on the same bus. It reports in by itself when it
       finds one; on a machine without Bluetooth this costs one walk
       across the ports and says nothing. */
    bt_init();

    /* Sound. Not needed by anything and that is exactly why it is
       started here rather than on demand: a machine that cannot make a
       noise should say so at startup, not the first time somebody
       asks for one. */
    if (hda_init())
        printk("[ OK ] Sound: Intel HDA, %s\n", hda_name());
    else
        printk("[ -- ] Sound: %s\n", hda_name());

    /* The IP address comes from the network, and the DHCP client waits
       for the server's replies - so it has to run in a thread, that is,
       after the scheduler */
    if (net) {
        dhcp_start();
        tcp_init();
    }

    printk("\nThe 64-bit system is ready - type 'help'.\n");
    shell64_start();

    /* 'desktop' on the boot line: straight to the desktop, the way a
       machine is used rather than the way it is tested. Without it the
       system stays at the shell, where every test expects to find it;
       'desktop' typed there does the same thing later. */
    if (boot_word(mbi, "desktop")) {
        printk("The desktop is starting - the console goes away\n");
        desktop_start();
    } else if (boot_word(mbi, "install")) {
        /* 'install': the installer on the whole screen, as if 'install'
           had been typed - the disc's menu entry for a person who came
           to install, not to read a shell's prompt */
        printk("The installer is starting\n");
        install_graphical();
    }

    /* From this moment on the main thread becomes the idle thread - the
       work is taken over by the shell, which waits for keys. Such a
       thread must not hold the big kernel lock: a core sitting on hlt
       with the lock in hand would close the kernel off to everyone
       else. */
    biglock_leave();
    for (;;)
        __asm__ volatile("hlt");
}
