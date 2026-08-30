/* A simple kernel shell
 *
 * It buffers characters from the keyboard until the end of a line and
 * runs the command. The shell is an ordinary kernel thread: the
 * keyboard interrupt only drops a character into a ring buffer and
 * wakes the thread, while all the rest - echo, line editing and
 * running the command - happens with interrupts enabled. Thanks to
 * that a command may sleep, read the disk or wait for the network
 * without blocking the timer or the rest of the system.
 */

#include <stdint.h>
#include "include/shell.h"
#include "include/keyboard.h"
#include "include/printk.h"
#include "include/vga.h"
#include "include/string.h"
#include "include/pmm.h"
#include "include/paging.h"
#include "include/swap.h"
#include "include/acpi.h"
#include "include/prog.h"
#include "include/smp.h"
#include "include/smpmem.h"
#include "include/smpthreads.h"
#include "include/pit.h"
#include "include/heap.h"
#include "include/task.h"
#include "include/initrd.h"
#include "include/exec.h"
#include "include/fs.h"
#include "include/rtc.h"
#include "include/mouse.h"
#include "include/video.h"
#include "include/desktop.h"
#include "include/io.h"
#include "include/net.h"
#include "include/dhcp.h"
#include "include/tcp.h"
#include "include/signal.h"

#define LINE_MAX 80

/* Every virtual console has its own shell: its own line being edited,
   its own key buffer and its own thread. The buffer size must be a
   power of two - indices are taken modulo a mask. */
#define SHELL_BUF 128

struct shell {
    char line[LINE_MAX];
    int line_len;
    char keybuf[SHELL_BUF];
    uint32_t kb_head, kb_tail;
    struct task *waiter;            /* != NULL only while the thread sleeps */
};

static struct shell shells[CONSOLE_COUNT];

/* The shell of the console the current task writes to */
static struct shell *my_shell(void)
{
    int vc = task_vc();
    return &shells[(vc >= 0 && vc < CONSOLE_COUNT) ? vc : 0];
}

static void prompt(void)
{
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    printk("> ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* The command list grows and the screen has 25 rows - hence two
   columns: the whole listing fits without scrolling. */
static const struct {
    const char *name;
    const char *desc;
} commands[] = {
    {"help",     "ta pomoc"},
    {"ps",       "watki jadra"},
    {"ls",       "pliki w initrd"},
    {"run X",    "uruchom program X"},
    {"fls",      "pliki na dysku"},
    {"cat F",    "wypisz plik"},
    {"write F",  "zapisz tekst do pliku"},
    {"cp A B",   "przepisz plik na dysk"},
    {"smp",      "policz cos na wszystkich rdzeniach"},
    {"smpmem",   "proba zamkow: pamiec z wszystkich rdzeni"},
    {"smpthreads", "watki jadra na wszystkich rdzeniach"},
    {"runw",     "jak run, ale program moze biec na kazdym rdzeniu"},
    {"df",       "zajetosc dysku"},
    {"mem",      "uzycie pamieci"},
    {"uptime",   "czas od startu"},
    {"date",     "data i godzina (RTC)"},
    {"mouse",    "pozycja myszy"},
    {"ifconfig", "karta sieciowa"},
    {"arp",      "tablica ARP"},
    {"ping IP",  "4 pakiety ICMP"},
    {"dns IP",   "ustaw serwer DNS"},
    {"netstat",  "polaczenia TCP"},
    {"dhcp",     "adres IP z sieci"},
    {"desktop",  "pulpit graficzny"},
    {"kill P S", "sygnal do procesu (dom. zabij)"},
    {"clear",    "wyczysc ekran"},
    {"gfx",      "test trybu 13h"},
    {"vesa",     "test trybu VESA"},
    {"crash",    "test page faulta"},
    {"reboot",   "restart komputera"},
    {"poweroff", "wylacz komputer"},
};

#define COMMAND_N ((int)(sizeof(commands) / sizeof(commands[0])))

static void print_padded(const char *s, int width)
{
    printk("%s", s);
    for (int i = (int)strlen(s); i < width; i++)
        printk(" ");
}

static void cmd_help(void)
{
    int rows = (COMMAND_N + 1) / 2;
    printk("Dostepne komendy:\n");
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < 2; c++) {
            int i = r + c * rows;
            if (i >= COMMAND_N)
                break;
            printk("  ");
            print_padded(commands[i].name, 10);
            print_padded(commands[i].desc, c ? 0 : 25);
        }
        printk("\n");
    }
    printk("Konsole: Alt+F1..F%d (teraz %d)\n",
           CONSOLE_COUNT, task_vc() + 1);
}

static void cmd_mem(void)
{
    printk("RAM wykryty:  %u KiB\n", pmm_ram_kib());
    printk("PMM:          %u/%u ramek 4 KiB zajetych (zarzadzane %u MiB)\n",
           pmm_used_frames(), pmm_total_frames(),
           pmm_managed_top() / (1024 * 1024));
    printk("sterta jadra: %u/%u B zajetych\n",
           heap_used_bytes(), heap_total_bytes());

    printk("z plikow:     %u stron wczytanych z odwzorowan\n",
           process_mmap_pages());

    uint32_t sused, stotal, sout, sin;
    swap_stats(&sused, &stotal, &sout, &sin);
    if (stotal)
        printk("wymiana:      %u/%u stron na dysku (wyrzuconych %u, "
               "sciagnietych %u)\n", sused, stotal, sout, sin);
    else
        printk("wymiana:      brak pliku wymiany\n");
}

static void cmd_kill(char *arg)
{
    if (!*arg) {
        printk("uzycie: kill <pid|nazwa> [sygnal], np. kill 7 15\n");
        return;
    }

    /* An optional signal number after the name/pid; without it - SIGKILL */
    int sig = SIGKILL;
    char *sp = arg;
    while (*sp && *sp != ' ')
        sp++;
    if (*sp == ' ') {
        *sp++ = '\0';
        while (*sp == ' ')
            sp++;
        sig = 0;
        for (const char *p = sp; *p >= '0' && *p <= '9'; p++)
            sig = sig * 10 + (*p - '0');
        if (sig <= 0 || sig >= NSIG) {
            printk("kill: nieznany sygnal '%s'\n", sp);
            return;
        }
    }

    /* A name instead of a pid: handier when a program was launched
       from the desktop menu, or when pid numbering depends on the
       order of startup */
    int pid;
    if (*arg < '0' || *arg > '9') {
        pid = task_find_name(arg);
        if (pid < 0) {
            printk("kill: nie ma procesu '%s'\n", arg);
            return;
        }
        printk("kill: proces '%s' ma pid %d\n", arg, pid);
    } else {
        pid = 0;
        for (const char *p = arg; *p >= '0' && *p <= '9'; p++)
            pid = pid * 10 + (*p - '0');
    }

    if (task_signal_send(pid, sig) < 0)
        printk("kill: nie ma procesu o pid %d\n", pid);
    else if (sig != SIGKILL)
        printk("kill: %s -> pid %d\n", signal_name(sig), pid);
}

static void cmd_uptime(void)
{
    uint32_t s = pit_ticks() / 100;
    printk("uptime: %u s (%u tyknac timera po 10 ms)\n", s, pit_ticks());
}

static void cmd_gfx(void)
{
    /* Switch into graphics mode 13h and draw a test pattern.
       This is a one-way move - the text console disappears. */
    video_init();
    video_clear(3);                       /* blue background */
    video_fill_rect(120, 70, 80, 60, 1);  /* a red rectangle in the middle */
    video_fill_rect(10, 10, 40, 40, 2);   /* green in a corner */
    video_fill_rect(270, 10, 40, 40, 5);  /* yellow in the other corner */
}

static void cmd_vesa(void)
{
    /* VESA mode 640x480x32 plus a test pattern (one-way). */
    if (video_init_vesa(640, 480) < 0) {
        printk("vesa: brak framebuffera (PCI)\n");
        return;
    }
    video_clear(3);                        /* blue background */
    video_fill_rect(200, 150, 240, 180, 1); /* red in the middle */
    video_fill_rect(20, 20, 60, 60, 2);    /* green in a corner */
    video_draw_string(210, 235, "ProtonOS VESA 640x480", 4);
    video_present();
}

static void cmd_mouse(void)
{
    int x, y, b;
    mouse_get(&x, &y, &b);
    printk("mysz: x=%u y=%u przyciski=%u\n", (uint32_t)x, (uint32_t)y,
           (uint32_t)b);
}

static void cmd_date(void)
{
    struct rtc_time t;
    rtc_read(&t);
    printk("%u-%u%u-%u%u %u%u:%u%u:%u%u\n", t.year,
           t.month / 10, t.month % 10, t.day / 10, t.day % 10,
           t.hour / 10, t.hour % 10, t.min / 10, t.min % 10,
           t.sec / 10, t.sec % 10);
}

static void cmd_cat(const char *name)
{
    /* Files may be hundreds of kilobytes, so we read them in chunks
       instead of holding the whole file in a kernel buffer */
    int slot = fs_open(FS_ROOT, name, 0);
    if (slot < 0) {
        printk("cat: brak pliku '%s' (lista: 'fls')\n", name);
        return;
    }

    char buf[257];
    uint32_t off = 0;
    char last = '\n';
    for (;;) {
        int n = fs_read_at(slot, off, buf, sizeof(buf) - 1);
        if (n <= 0)
            break;
        buf[n] = '\0';
        printk("%s", buf);
        last = buf[n - 1];
        off += (uint32_t)n;
    }
    if (last != '\n')
        printk("\n");
}

static void cmd_df(void)
{
    uint32_t used, total;
    fs_usage(&used, &total);
    if (!total) {
        printk("df: brak dysku\n");
        return;
    }
    uint32_t kib = FS_BLOCK / 1024;
    printk("dysk: %u KiB, zajete %u KiB, wolne %u KiB (bloki po %u B)\n",
           total * kib, used * kib, (total - used) * kib, FS_BLOCK);
}

static void cmd_write(char *args)
{
    /* args = "name text..." */
    char *text = args;
    while (*text && *text != ' ')
        text++;
    if (*text == ' ')
        *text++ = '\0';

    if (!*args || !*text) {
        printk("uzycie: write <plik> <tekst>\n");
        return;
    }
    if (fs_write(args, text, strlen(text)) < 0)
        printk("write: blad zapisu '%s'\n", args);
    else
        printk("zapisano %u B do '%s'\n", (uint32_t)strlen(text), args);
}

/* cp <source> <destination> - copying a file onto the disk
 *
 * We look for the source where 'run' looks for it: in the initrd
 * first, then on the disk. Thanks to that a program shipped with the
 * kernel can be moved onto the disk with one command - and run from
 * there afterwards. */
static void cmd_cp(char *args)
{
    char *dst = args;
    while (*dst && *dst != ' ')
        dst++;
    if (*dst == ' ')
        *dst++ = '\0';
    while (*dst == ' ')
        dst++;

    if (!*args || !*dst) {
        printk("uzycie: cp <zrodlo> <cel>\n");
        return;
    }

    uint32_t size = 0;
    void *loaded = (void *)0;
    const void *image = prog_image(args, &size, &loaded);
    if (!image) {
        printk("cp: nie ma '%s'\n", args);
        return;
    }
    int ok = fs_write(dst, image, size) == 0;
    prog_release(loaded);

    if (ok)
        printk("cp: '%s' -> '%s' (%u B)\n", args, dst, size);
    else
        printk("cp: nie udalo sie zapisac '%s'\n", dst);
}

/* smp - spreading computation across all cores
 *
 * Every unit of work computes the checksum of a chunk of a
 * pseudo-random sequence - enough work to keep a core busy, and simple
 * enough that the result can be checked.
 *
 * We compare the result against the one computed on a single core. It
 * matches when every unit was computed exactly once and nobody
 * trampled somebody else's cell - and that is the right question to
 * ask about concurrency, not whether anything got computed at all.
 */

#define SMP_UNITS 64
#define SMP_STEPS    200000

static uint32_t smp_results[SMP_UNITS];

/* A linear congruential generator - always the same sequence for a
   given number, so the result does not depend on which core computed it */
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

    /* Every core's own timer. The boot core runs on the PIT and has
       zero here - secondary ones count their ticks in the local APIC,
       and that is the only proof that they get interrupts at all. */
    printk("Zegary rdzeni:");
    for (int i = 0; i < smp_cpus(); i++)
        printk(" rdzen%d=%u", i, smp_ticks(i));
    printk("\n");
}

static void cmd_dhcp(void)
{
    if (!dhcp_configure())
        printk("dhcp: brak odpowiedzi serwera\n");
}

static void cmd_dns(const char *arg)
{
    uint32_t ip;
    if (!net_parse_ip(arg, &ip)) {
        printk("uzycie: dns <adres ip serwera>\n");
        return;
    }
    net_set_dns(ip);
    printk("serwer DNS ustawiony na ");
    net_print_ip(ip);
    printk("\n");
}

/* Ping and DHCP wait for answers (task_sleep). The shell used to run
   in interrupt context and was not allowed to sleep - both commands
   therefore went into threads of their own. Now the shell is a thread
   and simply waits, while characters typed in the meantime wait in the
   keyboard buffer. */
static void cmd_ping(const char *arg)
{
    uint32_t ip;
    if (!net_parse_ip(arg, &ip)) {
        printk("uzycie: ping <adres ip>, np. ping 10.0.2.2\n");
        return;
    }
    net_ping(ip, 4);
}

static void run_command(char *cmd)
{
    if (!*cmd)
        return;

    /* Separate the first word from the argument */
    char *arg = cmd;
    while (*arg && *arg != ' ')
        arg++;
    if (*arg == ' ') {
        *arg++ = '\0';
        while (*arg == ' ')
            arg++;
    }

    if (!strcmp(cmd, "help"))
        cmd_help();
    else if (!strcmp(cmd, "ps"))
        task_dump();
    else if (!strcmp(cmd, "ls"))
        initrd_ls();
    else if (!strcmp(cmd, "run")) {
        if (*arg)
            exec_spawn(arg);
        else
            printk("uzycie: run <plik> (lista: 'ls')\n");
    }
    /* The same, but the program may run on any core. Its kernel side
       runs under the big lock anyway - what goes in parallel is the
       computing in ring 3. */
    else if (!strcmp(cmd, "runw")) {
        if (*arg)
            exec_spawn_anycore(arg);
        else
            printk("uzycie: runw <plik> (jak 'run', ale na kazdym "
                   "rdzeniu)\n");
    }
    else if (!strcmp(cmd, "fls"))
        fs_list();
    else if (!strcmp(cmd, "cat")) {
        if (*arg)
            cmd_cat(arg);
        else
            printk("uzycie: cat <plik>\n");
    }
    else if (!strcmp(cmd, "df"))
        cmd_df();
    else if (!strcmp(cmd, "write"))
        cmd_write(arg);
    else if (!strcmp(cmd, "cp"))
        cmd_cp(arg);
    else if (!strcmp(cmd, "smp"))
        cmd_smp();
    else if (!strcmp(cmd, "smpmem"))
        smpmem_test();
    else if (!strcmp(cmd, "smpthreads"))
        smpthreads_test();
    else if (!strcmp(cmd, "mem"))
        cmd_mem();
    else if (!strcmp(cmd, "uptime"))
        cmd_uptime();
    else if (!strcmp(cmd, "date"))
        cmd_date();
    else if (!strcmp(cmd, "mouse"))
        cmd_mouse();
    else if (!strcmp(cmd, "ifconfig"))
        net_ifconfig();
    else if (!strcmp(cmd, "arp"))
        net_arp_dump();
    else if (!strcmp(cmd, "netstat"))
        tcp_dump();
    else if (!strcmp(cmd, "dns"))
        cmd_dns(arg);
    else if (!strcmp(cmd, "dhcp"))
        cmd_dhcp();
    else if (!strcmp(cmd, "ping"))
        cmd_ping(arg);
    else if (!strcmp(cmd, "gfx"))
        cmd_gfx();
    else if (!strcmp(cmd, "vesa"))
        cmd_vesa();
    else if (!strcmp(cmd, "desktop"))
        desktop_start();
    else if (!strcmp(cmd, "kill"))
        cmd_kill(arg);
    else if (!strcmp(cmd, "clear"))
        vga_clear();
    else if (!strcmp(cmd, "crash"))
        *(volatile uint32_t *)0xE0000000 = 1;  /* outside the mapping -> #PF */
    else if (!strcmp(cmd, "reboot")) {
        printk("Restart...\n");
        if (!acpi_reboot())
            outb(0x64, 0xFE);  /* fallback: a pulse through the keyboard controller */
    }
    else if (!strcmp(cmd, "poweroff")) {
        printk("Wylaczanie zasilania...\n");
        acpi_poweroff();
        printk("poweroff: nie udalo sie - mozna wylaczyc zasilanie recznie\n");
    }
    else
        printk("nieznana komenda: '%s' (wpisz 'help')\n", cmd);
}

static void shell_handle_char(struct shell *sh, char c)
{
    if (c == '\n') {
        printk("\n");
        sh->line[sh->line_len] = '\0';
        run_command(sh->line);
        sh->line_len = 0;
        prompt();
        return;
    }
    if (c == '\b') {
        if (sh->line_len > 0) {
            sh->line_len--;
            printk("\b");
        }
        return;
    }
    if (c < 0x20)                       /* arrows and friends - not for the shell */
        return;
    if (sh->line_len < LINE_MAX - 1) {
        sh->line[sh->line_len++] = c;
        printk("%c", c);
    }
}

/* After an asynchronous kernel message (DHCP, a background ping) the
   prompt is left somewhere higher on the screen - redraw it together
   with the unfinished line. */
void shell_refresh(void)
{
    struct shell *sh = my_shell();
    prompt();
    sh->line[sh->line_len] = '\0';
    if (sh->line_len)
        printk("%s", sh->line);
}

/* Keyboard interrupt context (IF=0): only park the character and
   wake the thread. The character comes from the console currently
   visible. */
static void shell_on_char(char c)
{
    struct shell *sh = &shells[console_active()];
    if (sh->kb_head - sh->kb_tail < SHELL_BUF)
        sh->keybuf[sh->kb_head++ & (SHELL_BUF - 1)] = c;
    if (sh->waiter) {
        task_unblock(sh->waiter);
        sh->waiter = (struct task *)0;
    }
}

/* Thread context: waits for a character from its own console's buffer */
static char shell_getchar(struct shell *sh)
{
    uint32_t flags = irq_save();
    while (sh->kb_head == sh->kb_tail) {
        sh->waiter = task_current();
        task_block();
    }
    char c = sh->keybuf[sh->kb_tail++ & (SHELL_BUF - 1)];
    irq_restore(flags);
    return c;
}

static void shell_thread(void *arg)
{
    int vc = (int)arg;
    task_set_vc(vc);                    /* from now on we write to our own console */
    struct shell *sh = &shells[vc];

    /* The first console got its greeting back in shell_init, so that
       the startup messages and the prompt settle in a predictable
       order. The others greet themselves. */
    if (vc) {
        printk("ProtonOS - konsola %d. Alt+F1..F4 przelacza.\n", vc + 1);
        prompt();
    }

    for (;;)
        shell_handle_char(sh, shell_getchar(sh));
}

void shell_init(void)
{
    for (int vc = 0; vc < CONSOLE_COUNT; vc++)
        keyboard_set_handler(vc, shell_on_char);

    printk("\nProtonOS shell - wpisz 'help', by zobaczyc komendy.\n");
    prompt();

    for (int vc = 0; vc < CONSOLE_COUNT; vc++)
        task_create("shell", shell_thread, (void *)(uint32_t)vc);
}
