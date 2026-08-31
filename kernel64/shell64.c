/* The 64-bit kernel's shell
 *
 * An ordinary kernel thread: it reads characters from the keyboard,
 * assembles lines out of them and runs commands. There are no
 * interrupts in it and no global state beyond its own line buffer -
 * it is a plain loop that happens to run in parallel with the rest of
 * the system.
 *
 * There are as many commands as the kernel really knows how to do.
 * Each of them shows something that cannot be seen any other way than
 * by asking the system while it runs - and that is the whole
 * difference between a shell and a script printed out at startup.
 */

#include <stdint.h>
#include "shell64.h"
#include "keyboard64.h"
#include "task64.h"
#include "paging64.h"
#include "elf64.h"
#include "proc64.h"
#include "../kernel/include/prog.h"
#include "isr64.h"
#include "task64.h"
#include "syscall64.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/string.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/pit.h"
#include "../kernel/include/initrd.h"
#include "../kernel/include/blk.h"
#include "../kernel/include/acpi.h"
#include "../kernel/include/swap.h"
#include "../kernel/include/rtc.h"
#include "../kernel/include/smp.h"
#include "../kernel/include/smpmem.h"
#include "../kernel/include/smpthreads.h"
#include "../kernel/include/io.h"
#include "../kernel/include/fs.h"
#include "../kernel/include/fbcon.h"
#include "../kernel/include/net.h"
#include "../kernel/include/video.h"
#include "../kernel/include/desktop.h"
#include "../kernel/include/vga.h"

#define LINE_MAX 80

/* Does the line start with the given word? (the kernel has no strncmp
   - and has no reason to have one for a single place) */
static int starts_with(const char *s, const char *prefix)
{
    while (*prefix)
        if (*s++ != *prefix++)
            return 0;
    return 1;
}


static void prompt(void)
{
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    printk("proton64> ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_help(void)
{
    printk("Polecenia: help, ps, mem, ls, disk, fls, pwd, cd <kat>,\n");
    printk("           mkdir <kat>, cat <plik>, write <plik> <tresc>,\n");
    printk("           rm <plik>, cp <zrodlo> <cel>, uptime, date,\n");
    printk("           run <program>, clear,\n");
    printk("           ifconfig, arp, ping <adres>, dns <ip>,\n");
    printk("           gfx, pulpit,\n");
    printk("           reboot, poweroff, smp, smpmem, smpthreads\n");
    printk("           runw <prog> - program na dowolnym rdzeniu\n");
}

static void cmd_mem(void)
{
    uint32_t total = pmm_total_frames();
    uint32_t used = pmm_used_frames();
    printk("Pamiec: %u KiB RAM, ramki %u/%u zajete (%u KiB wolne)\n",
           pmm_ram_kib(), used, total, (total - used) * 4);
    printk("Stronicowanie: identycznosciowo %u MiB, rozdzielonych stron %u\n",
           (uint32_t)(paging64_identity_top() / (1024 * 1024)),
           paging64_cow_splits());

    uint32_t wuz, not_at_all, wout, win;
    swap_stats(&wuz, &not_at_all, &wout, &win);
    if (not_at_all)
        printk("Wymiana: %u/%u stron na dysku "
               "(wyrzuconych %u, sciagnietych %u)\n", wuz, not_at_all, wout, win);
    else
        printk("Wymiana: brak pliku wymiany\n");
}

/* Writing and reading a sector - the only way to check the disk for
   real rather than merely that the controller introduced itself.
   The last sector: nothing lies there yet. */
static void cmd_disk(void)
{
    if (!blk_present()) {
        printk("disk: brak dysku\n");
        return;
    }
    printk("Dysk: %s, %u sektorow (%u MiB)\n",
           blk_name(), blk_sectors(), blk_sectors() / 2048);

    static uint8_t buf[512];
    uint32_t lba = blk_sectors() ? blk_sectors() - 1 : 0;

    for (int i = 0; i < 512; i++)
        buf[i] = (uint8_t)(i + 7);
    if (blk_write(lba, 1, buf) < 0) {
        printk("disk: zapis sektora %u nie przeszedl\n", lba);
        return;
    }

    for (int i = 0; i < 512; i++)
        buf[i] = 0;
    if (blk_read(lba, 1, buf) < 0) {
        printk("disk: odczyt sektora %u nie przeszedl\n", lba);
        return;
    }

    int differ = 0;
    for (int i = 0; i < 512; i++)
        if (buf[i] != (uint8_t)(i + 7))
            differ++;
    printk("disk: sektor %u zapisany i odczytany, roznic %d\n", lba, differ);
}

/* --- File system --------------------------------------------------
 *
 * Three commands are enough to check whether a file really lies on the
 * disk and not only in memory: write it, print it, see it in the
 * listing. The fourth proof arrives by itself - after a restart.
 */
static void cmd_pwd(void)
{
    char path[FS_NAME_MAX * 4];
    fs_getpath(task64_cwd(), path);
    printk("%s\n", path);
}

static void cmd_fls(void)
{
    if (!fs_ready()) {
        printk("fls: brak systemu plikow\n");
        return;
    }

    /* We list the directory the shell is standing in - not always the
   root one */
    char name[FS_NAME_MAX + 1];
    uint32_t size = 0;
    int dir = 0;
    cmd_pwd();
    for (int i = 0; fs_readdir(task64_cwd(), i, name,
                               &size, &dir) == 1; i++)
        printk("  %s%s  (%u B)\n", name, dir ? "/" : "", size);

    uint32_t used = 0, total = 0;
    fs_usage(&used, &total);
    printk("Zajetosc: %u/%u blokow po %u B\n", used, total, FS_BLOCK);
}

static void cmd_cat(const char *name)
{
    static char buf[512];

    if (!fs_ready()) {
        printk("cat: brak systemu plikow\n");
        return;
    }
    int slot = fs_open(task64_cwd(), name, 0);
    int n = slot < 0 ? -1 : fs_read_at(slot, 0, buf, sizeof(buf) - 1);
    if (n < 0) {
        printk("cat: nie ma pliku '%s'\n", name);
        return;
    }
    buf[n] = '\0';
    printk("%s", buf);
    if (n && buf[n - 1] != '\n')
        printk("\n");
}

/* write <file> <text> - the rest of the line goes into the file */
static void cmd_write(char *rest)
{
    if (!fs_ready()) {
        printk("write: brak systemu plikow\n");
        return;
    }

    char *body = rest;
    while (*body && *body != ' ')
        body++;
    if (!*body) {
        printk("write: podaj nazwe i tresc\n");
        return;
    }
    *body++ = '\0';                 /* the name ends here */
    while (*body == ' ')
        body++;

    uint32_t len = 0;
    while (body[len])
        len++;
    body[len++] = '\n';             /* a text file ends with a newline */

    int slot = fs_open(task64_cwd(), rest, 1);
    if (slot < 0 || fs_truncate(slot) < 0 ||
        fs_write_at(slot, 0, body, len) < 0)
        printk("write: nie udalo sie zapisac '%s'\n", rest);
    else
        printk("write: '%s' zapisany (%u B)\n", rest, len);
}

/* smp - spreading the counting across all cores
 *
 * Every work item computes a checksum over a piece of a pseudorandom
 * sequence - enough work for a core to have something to do, and
 * simple enough that the result can be verified.
 *
 * We compare the result with one computed on a single core. It matches
 * when every item was computed exactly once and nobody trampled
 * somebody else's cell - and that is the right question to ask about
 * concurrency, not whether anything got computed at all.
 */

#define SMP_UNITS 64
#define SMP_STEPS    200000

static uint32_t smp_results[SMP_UNITS];

/* A linear congruential generator - always the same sequence for a
   given number, so the result does not depend on which core computed
   it */
static uint32_t smp_run_chunk(uint32_t number)
{
    uint32_t stan = number * 2654435761u + 12345u;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < SMP_STEPS; i++) {
        stan = stan * 1103515245u + 12345u;
        sum += stan >> 16;
    }
    return sum;
}

static void smp_unit(uint32_t number, void *data)
{
    (void)data;
    if (number < SMP_UNITS)
        smp_results[number] = smp_run_chunk(number);
}

static void cmd_smp(void)
{
    uint32_t pattern = 0;
    for (uint32_t i = 0; i < SMP_UNITS; i++)
        pattern += smp_run_chunk(i);

    for (uint32_t i = 0; i < SMP_UNITS; i++)
        smp_results[i] = 0;
    smp_work(smp_unit, 0, SMP_UNITS);

    uint32_t total = 0;
    for (uint32_t i = 0; i < SMP_UNITS; i++)
        total += smp_results[i];

    printk("Rdzeni: %d (obudzonych %d), jednostek %u\n",
           smp_cpus(), smp_started(), (uint32_t)SMP_UNITS);
    printk("Udzial:");
    for (int i = 0; i < smp_cpus(); i++)
        printk(" rdzen%d=%u", i, smp_share(i));
    printk("\n");
    printk("Wynik: %u, wzorzec: %u - %s\n", total, pattern,
           total == pattern ? "zgadza sie" : "NIE ZGADZA SIE");
    printk("Liczylo rdzeni: %d\n", smp_workers());

    /* Every core's own clock. The bootstrap core runs off the PIT and
       has zero here - the secondary ones count their own ticks in the
       local APIC, and that is the only proof that they receive
       interrupts at all. */
    printk("Zegary rdzeni:");
    for (int i = 0; i < smp_cpus(); i++)
        printk(" rdzen%d=%u", i, smp_ticks(i));
    printk("\n");
}

/* cp <source> <target> - copying a file onto the disk
 *
 * We look for the source where 'run' looks for it: first in the
 * initrd, then on the disk. Thanks to that a program shipped together
 * with the kernel can be moved onto the disk with a single command -
 * and run from there from then on, including when it is no longer in
 * the initrd. */
static void cmd_cp(char *rest)
{
    if (!fs_ready()) {
        printk("cp: brak systemu plikow\n");
        return;
    }

    char *dst = rest;
    while (*dst && *dst != ' ')
        dst++;
    if (!*dst) {
        printk("cp: podaj zrodlo i cel, np. 'cp hello64 hello64'\n");
        return;
    }
    *dst++ = '\0';                    /* the source name ends here */
    while (*dst == ' ')
        dst++;
    if (!*dst) {
        printk("cp: podaj cel\n");
        return;
    }

    uint32_t size = 0;
    void *loaded = (void *)0;
    const void *image = prog_image(rest, &size, &loaded);
    if (!image) {
        printk("cp: nie ma '%s'\n", rest);
        return;
    }

    /* fs_write_at returns the NUMBER of bytes written, not zero - a
       file cut off halfway has to count as an error too */
    int slot = fs_open(task64_cwd(), dst, 1);
    int ok = slot >= 0 && fs_truncate(slot) >= 0 &&
             fs_write_at(slot, 0, image, size) == (int)size;
    prog_release(loaded);

    if (ok)
        printk("cp: '%s' -> '%s' (%u B)\n", rest, dst, size);
    else
        printk("cp: nie udalo sie zapisac '%s'\n", dst);
}

/* A drawing that checks graphics mode.
 *
 * Four rectangles in the corners, a frame and a caption - that is
 * enough to see whether the colours land where they should and
 * whether the pixel at (x, y) ends up at (x, y). A wrongly computed
 * bytes-per-pixel shows up here immediately: the image skews.
 */
static void cmd_gfx(void)
{
    if (!video_w()) {
        printk("gfx: brak trybu graficznego\n");
        return;
    }
    int w = video_w(), h = video_h();

    /* The message goes BEFORE the drawing: the console and the drawing
       share the same buffer and every print redraws it in full - so
       anything written after the drawing lands on top of it. */
    printk("gfx: rysuje %ux%u, %u bitow na piksel\n",
           (uint32_t)w, (uint32_t)h, (uint32_t)fbcon_bpp());

    /* video.c's palette defines indices 0-5, not the sixteen colours of
       text mode: 1 red, 2 green, 3 blue, 4 white, 5 yellow. */
    video_clear(3);                                    /* blue background */
    video_fill_rect(0, 0, w / 4, h / 4, 1);            /* top left     */
    video_fill_rect(w - w / 4, 0, w / 4, h / 4, 2);    /* top right    */
    video_fill_rect(0, h - h / 4, w / 4, h / 4, 5);    /* bottom left  */
    video_fill_rect(w - w / 4, h - h / 4, w / 4, h / 4, 4);

    for (int x = 0; x < w; x++) {
        video_putpixel(x, 0, 4);
        video_putpixel(x, h - 1, 4);
    }
    for (int y = 0; y < h; y++) {
        video_putpixel(0, y, 4);
        video_putpixel(w - 1, y, 4);
    }
    video_draw_string(w / 2 - 112, h / 2, "ProtonOS 64 - tryb graficzny", 4);
    video_present();
}

/* date - the time from the motherboard clock */
static void cmd_date(void)
{
    struct rtc_time t;
    rtc_read(&t);
    printk("%u-%u-%u %u:%u:%u\n", (uint32_t)t.year, (uint32_t)t.month,
           (uint32_t)t.day, (uint32_t)t.hour, (uint32_t)t.min,
           (uint32_t)t.sec);
}

static void cmd_uptime(void)
{
    uint32_t t = pit_ticks();
    printk("Uptime: %u s (%u tyknac po 10 ms)\n", t / 100, t);
}

/* The command's argument: the first word after the name, with no
   space before it */
static const char *argument(const char *line, int cmd_len)
{
    int i = cmd_len;
    while (line[i] == ' ')
        i++;
    return line[i] ? &line[i] : (const char *)0;
}

static void execute(const char *line)
{
    if (!line[0])
        return;

    if (!strcmp(line, "help")) {
        cmd_help();
    } else if (!strcmp(line, "ps")) {
        task64_dump();
    } else if (!strcmp(line, "mem")) {
        cmd_mem();
    } else if (!strcmp(line, "ls")) {
        initrd_ls();
    } else if (!strcmp(line, "fls")) {
        cmd_fls();
    } else if (!strcmp(line, "pwd")) {
        cmd_pwd();
    } else if (starts_with(line, "cd ")) {
        const char *kat = argument(line, 3);
        int fresh_one = FS_ROOT;
        if (!kat)
            printk("cd: podaj katalog\n");
        else if (fs_chdir(task64_cwd(), kat, &fresh_one) < 0)
            printk("cd: nie ma katalogu '%s'\n", kat);
        else
            task64_set_cwd(fresh_one);
    } else if (starts_with(line, "mkdir ")) {
        const char *kat = argument(line, 6);
        if (!kat)
            printk("mkdir: podaj nazwe\n");
        else if (fs_mkdir(task64_cwd(), kat) < 0)
            printk("mkdir: nie udalo sie zalozyc '%s'\n", kat);
        else
            printk("mkdir: '%s' zalozony\n", kat);
    } else if (starts_with(line, "rm ")) {
        const char *file = argument(line, 3);
        if (!file)
            printk("rm: podaj nazwe\n");
        else if (fs_unlink(task64_cwd(), file) < 0)
            printk("rm: nie ma '%s'\n", file);
        else
            printk("rm: '%s' usuniety\n", file);
    } else if (starts_with(line, "cat ")) {
        const char *name = argument(line, 4);
        if (name)
            cmd_cat(name);
        else
            printk("cat: podaj nazwe pliku\n");
    } else if (!strcmp(line, "smp")) {
        cmd_smp();
    } else if (!strcmp(line, "smpmem")) {
        smpmem_test();
    } else if (!strcmp(line, "smpthreads")) {
        smpthreads_test();
    } else if (starts_with(line, "cp ")) {
        char *arg = (char *)argument(line, 3);
        if (!arg)
            printk("cp: podaj zrodlo i cel\n");
        else
            cmd_cp(arg);
    } else if (starts_with(line, "write ")) {
        char *rest = (char *)argument(line, 6);
        if (rest)
            cmd_write(rest);
        else
            printk("write: podaj nazwe i tresc\n");
    } else if (!strcmp(line, "pulpit")) {
        /* Switching to graphics mode is one-way: from this moment the
           desktop takes over the keyboard and the text console stops
           being visible */
        printk("Pulpit startuje - konsola znika\n");
        desktop_start();
    } else if (!strcmp(line, "gfx")) {
        cmd_gfx();
    } else if (!strcmp(line, "ifconfig")) {
        net_ifconfig();
    } else if (!strcmp(line, "arp")) {
        net_arp_dump();
    } else if (starts_with(line, "dns ")) {
        /* Setting the name server by hand - DHCP supplies one, but it is
           not always the one that was meant */
        const char *addr = argument(line, 4);
        uint32_t ip = 0;
        if (!addr || !net_parse_ip(addr, &ip)) {
            printk("uzycie: dns <adres ip serwera>\n");
        } else {
            net_set_dns(ip);
            printk("serwer DNS ustawiony na ");
            net_print_ip(ip);
            printk("\n");
        }
    } else if (starts_with(line, "ping ")) {
        const char *addr = argument(line, 5);
        uint32_t ip = 0;
        if (!addr)
            printk("ping: podaj adres\n");
        else if (!net_parse_ip(addr, &ip))
            printk("ping: '%s' to nie jest adres IP\n", addr);
        else
            net_ping(ip, 3);
    } else if (!strcmp(line, "disk")) {
        cmd_disk();
    } else if (!strcmp(line, "uptime")) {
        cmd_uptime();
    } else if (!strcmp(line, "date")) {
        cmd_date();
    } else if (!strcmp(line, "clear")) {
        vga_clear();
    } else if (!strcmp(line, "reboot")) {
        printk("Restart...\n");
        /* When the ACPI tables have no reset register, there is a way
           older than they are: a pulse through the keyboard
           controller. */
        if (!acpi_reboot())
            outb(0x64, 0xFE);
    } else if (!strcmp(line, "poweroff")) {
        printk("Wylaczanie zasilania...\n");
        acpi_poweroff();
        printk("poweroff: nie udalo sie - "
               "mozna wylaczyc zasilanie recznie\n");
    } else if (starts_with(line, "runw ")) {
        /* Like 'run', but the program may run on any core - and we do not
           wait for it, so that several can be started at once. Its
           kernel side runs under the big lock anyway; what goes in
           parallel is the counting in ring 3. */
        const char *name = argument(line, 5);
        if (!name) {
            printk("runw: podaj nazwe programu (zobacz 'ls')\n");
            return;
        }
        static char progw[16];
        int pw = 0;
        while (name[pw] && name[pw] != ' ' && pw < (int)sizeof(progw) - 1) {
            progw[pw] = name[pw];
            pw++;
        }
        progw[pw] = '\0';
        if (!prog_exists(progw)) {
            printk("runw: nie ma programu '%s'\n", progw);
            return;
        }
        if (proc64_spawn_anycore(progw, 0, 1) < 0)
            printk("runw: nie udalo sie uruchomic '%s'\n", progw);
    } else if (starts_with(line, "run ")) {
        const char *name = argument(line, 4);
        if (!name) {
            printk("run: podaj nazwe programu (zobacz 'ls')\n");
            return;
        }
        /* The name is the first word, the rest are arguments */
        static char prog[16];
        int pn = 0;
        while (name[pn] && name[pn] != ' ' && pn < (int)sizeof(prog) - 1) {
            prog[pn] = name[pn];
            pn++;
        }
        prog[pn] = '\0';
        name = prog;

        if (!prog_exists(name)) {
            printk("run: nie ma programu '%s' "
                   "(zobacz 'ls' i 'fls')\n", name);
            return;
        }
        /* The rest of the line is the program's arguments - the shell
           only splits it into words, and what comes of them is the
           program's business */
        static struct args64 args;
        args.argc = 0;
        for (int i = 4; line[i] && args.argc < ARG_MAX; ) {
            while (line[i] == ' ')
                i++;
            if (!line[i])
                break;
            int k = 0;
            while (line[i] && line[i] != ' ' && k < ARG_LEN - 1)
                args.v[args.argc][k++] = line[i++];
            args.v[args.argc][k] = '\0';
            args.argc++;
            while (line[i] && line[i] != ' ')
                i++;
        }

        uint32_t frames_before = pmm_used_frames();
        uint32_t pages_before = isr64_heap_pages();
        uint32_t stack_before = isr64_stack_pages();
        uint32_t files_before = isr64_mmap_pages();
        int id = proc64_spawn(name, args.argc ? &args : 0, 0);
        if (id < 0) {
            printk("run: nie udalo sie uruchomic '%s'\n", name);
            return;
        }
        /* The shell waits for its child - exactly as a program would do
           through a system call. It does not respond in the meantime,
           and that is how it should be. */
        keyboard64_set_intr_target(id);   /* from now on Ctrl+C goes to it */
        int code = 0;
        int done = task64_wait(&code);
        keyboard64_set_intr_target(-1);
        if (done < 0)
            printk("run: dziecko przepadlo\n");
        else
            printk("[%s] koniec, kod wyjscia %d (ramek %u -> %u, "
                   "stron sterty %u, stosu %u, z plikow %u)\n",
                   name, code, frames_before, pmm_used_frames(),
                   isr64_heap_pages() - pages_before,
                   isr64_stack_pages() - stack_before,
                   isr64_mmap_pages() - files_before);
    } else {
        printk("Nieznane polecenie: %s (sprobuj 'help')\n", line);
    }
}

static void shell_thread(void *arg)
{
    int vc = (int)(long)arg;
    task64_set_vc(vc);          /* from now on we write on our own console */

    /* The command line belongs to the thread, not to the kernel: every
       console has its own shell, so every one has its own buffer too -
       on its own stack, with no tables indexed by console number. */
    char line[LINE_MAX];
    int line_len = 0;

    /* The first console gets a full greeting with the command list, the
       others a short note: four listings in a row on the serial port
       are of no use to anyone. */
    if (vc == 0) {
        printk("\n");
        cmd_help();
    } else {
        printk("ProtonOS 64 - konsola %d. Alt+F1..F4 przelacza.\n", vc + 1);
    }
    prompt();

    for (;;) {
        char c = keyboard64_getchar();

        if (c == '\n') {
            printk("\n");
            line[line_len] = '\0';
            execute(line);
            line_len = 0;
            prompt();
        } else if (c == '\b') {
            if (line_len) {
                line_len--;
                printk("\b");
            }
        } else if (c >= ' ' && c < 0x7F && line_len < LINE_MAX - 1) {
            line[line_len++] = c;
            printk("%c", c);
        }
        /* Control keys (arrows, Ctrl+C) are skipped by the shell for now */
    }
}

void shell64_start(void)
{
    for (int vc = 0; vc < CONSOLE_COUNT; vc++)
        task64_create("shell", shell_thread, (void *)(long)vc);
}
