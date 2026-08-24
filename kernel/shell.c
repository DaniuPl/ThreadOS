/* Prosty shell jądra
 *
 * Buforuje znaki z klawiatury do końca linii i wykonuje komendę.
 * Działa w kontekście przerwania klawiatury - dla zabawkowego systemu
 * w zupełności wystarczy, dopóki komendy są krótkie.
 */

#include <stdint.h>
#include "include/shell.h"
#include "include/keyboard.h"
#include "include/printk.h"
#include "include/vga.h"
#include "include/string.h"
#include "include/pmm.h"
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

#define LINE_MAX 80

static char line[LINE_MAX];
static int line_len;

static void prompt(void)
{
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    printk("> ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_help(void)
{
    printk("Dostepne komendy:\n"
           "  help    - ta pomoc\n"
           "  ps      - lista watkow jadra\n"
           "  ls      - pliki w initrd\n"
           "  run X   - uruchom program X z initrd (proces ring 3)\n"
           "  fls     - pliki na dysku (PFS)\n"
           "  cat F   - wypisz plik z dysku\n"
           "  write F txt - zapisz tekst do pliku na dysku\n"
           "  mem     - uzycie pamieci (RAM, ramki PMM, sterta)\n"
           "  uptime  - czas od startu systemu\n"
           "  date    - data i godzina z RTC\n"
           "  mouse   - pozycja i przyciski myszy\n"
           "  ifconfig- karta sieciowa: MAC, IP, statystyki\n"
           "  arp     - tablica ARP (IP -> MAC)\n"
           "  ping IP - wyslij 4 pakiety ICMP echo, np. ping 10.0.2.2\n"
           "  dhcp    - pobierz adres IP z sieci (DHCP)\n"
           "  gfx     - tryb graficzny 13h, wzorzec testowy\n"
           "  desktop - graficzny pulpit z kursorem myszy\n"
           "  kill P  - zabij proces (pid albo nazwa)\n"
           "  clear   - wyczysc ekran\n"
           "  crash   - test page faulta (zatrzymuje system!)\n"
           "  reboot  - restart komputera\n");
}

static void cmd_mem(void)
{
    printk("RAM wykryty:  %u KiB\n", pmm_ram_kib());
    printk("PMM:          %u/%u ramek 4 KiB zajetych (zarzadzane 64 MiB)\n",
           pmm_used_frames(), pmm_total_frames());
    printk("sterta jadra: %u/%u B zajetych\n",
           heap_used_bytes(), heap_total_bytes());
}

static void cmd_kill(const char *arg)
{
    if (!*arg) {
        printk("uzycie: kill <pid|nazwa>\n");
        return;
    }

    /* Nazwa zamiast pidu: wygodniejsza, gdy program poszedl z menu
       pulpitu albo gdy numeracja pidow zalezy od kolejnosci startu */
    if (*arg < '0' || *arg > '9') {
        int pid = task_kill_name(arg);
        if (pid < 0)
            printk("kill: nie ma procesu '%s'\n", arg);
        else
            printk("kill: proces '%s' ma pid %d\n", arg, pid);
        return;
    }

    int pid = 0;
    for (const char *p = arg; *p >= '0' && *p <= '9'; p++)
        pid = pid * 10 + (*p - '0');
    if (task_kill(pid) < 0)
        printk("kill: nie ma procesu o pid %d\n", pid);
}

static void cmd_uptime(void)
{
    uint32_t s = pit_ticks() / 100;
    printk("uptime: %u s (%u tyknac timera po 10 ms)\n", s, pit_ticks());
}

static void cmd_gfx(void)
{
    /* Przelacz w tryb graficzny 13h i narysuj wzorzec testowy.
       To przejscie jednokierunkowe - tekstowa konsola znika. */
    video_init();
    video_clear(3);                       /* tlo niebieskie */
    video_fill_rect(120, 70, 80, 60, 1);  /* czerwony prostokat na srodku */
    video_fill_rect(10, 10, 40, 40, 2);   /* zielony w rogu */
    video_fill_rect(270, 10, 40, 40, 5);  /* zolty w drugim rogu */
}

static void cmd_vesa(void)
{
    /* Tryb VESA 640x480x32 + wzorzec testowy (jednokierunkowe). */
    if (video_init_vesa(640, 480) < 0) {
        printk("vesa: brak framebuffera (PCI)\n");
        return;
    }
    video_clear(3);                        /* tlo niebieskie */
    video_fill_rect(200, 150, 240, 180, 1); /* czerwony na srodku */
    video_fill_rect(20, 20, 60, 60, 2);    /* zielony w rogu */
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
    static char buf[FS_MAX_SIZE + 1];
    int n = fs_read(name, buf, FS_MAX_SIZE);
    if (n < 0) {
        printk("cat: brak pliku '%s' (lista: 'fls')\n", name);
        return;
    }
    buf[n] = '\0';
    printk("%s", buf);
    if (n && buf[n - 1] != '\n')
        printk("\n");
}

static void cmd_write(char *args)
{
    /* args = "nazwa tekst..." */
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

/* Ping czeka na odpowiedzi (task_sleep), a shell dziala w kontekscie
   przerwania klawiatury - dlatego robota idzie do osobnego watku jadra. */
static uint32_t ping_target;

static void ping_thread(void *arg)
{
    (void)arg;
    net_ping(ping_target, 4);
    shell_refresh();
}

static void dhcp_thread_cmd(void *arg)
{
    (void)arg;
    if (!dhcp_configure())
        printk("dhcp: brak odpowiedzi serwera\n");
    shell_refresh();
}

static void cmd_ping(const char *arg)
{
    uint32_t ip;
    if (!net_parse_ip(arg, &ip)) {
        printk("uzycie: ping <adres ip>, np. ping 10.0.2.2\n");
        return;
    }
    ping_target = ip;
    task_create("ping", ping_thread, 0);
}

static void run_command(char *cmd)
{
    if (!*cmd)
        return;

    /* Oddziel pierwsze slowo od argumentu */
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
    else if (!strcmp(cmd, "fls"))
        fs_list();
    else if (!strcmp(cmd, "cat")) {
        if (*arg)
            cmd_cat(arg);
        else
            printk("uzycie: cat <plik>\n");
    }
    else if (!strcmp(cmd, "write"))
        cmd_write(arg);
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
    else if (!strcmp(cmd, "dhcp"))
        task_create("dhcp", dhcp_thread_cmd, 0);
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
        *(volatile uint32_t *)0xE0000000 = 1;  /* poza mapowaniem -> #PF */
    else if (!strcmp(cmd, "reboot"))
        outb(0x64, 0xFE);  /* impuls reset przez kontroler klawiatury */
    else
        printk("nieznana komenda: '%s' (wpisz 'help')\n", cmd);
}

static void shell_on_char(char c)
{
    if (c == '\n') {
        printk("\n");
        line[line_len] = '\0';
        run_command(line);
        line_len = 0;
        prompt();
        return;
    }
    if (c == '\b') {
        if (line_len > 0) {
            line_len--;
            printk("\b");
        }
        return;
    }
    if (line_len < LINE_MAX - 1) {
        line[line_len++] = c;
        printk("%c", c);
    }
}

/* Po asynchronicznym wypisie jadra (DHCP, ping w tle) prompt zostaje
   gdzies wyzej na ekranie - odrysuj go razem z niedokonczona linia. */
void shell_refresh(void)
{
    prompt();
    line[line_len] = '\0';
    if (line_len)
        printk("%s", line);
}

void shell_init(void)
{
    keyboard_set_handler(shell_on_char);
    printk("\nProtonOS shell - wpisz 'help', by zobaczyc komendy.\n");
    prompt();
}
