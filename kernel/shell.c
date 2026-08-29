/* Prosty shell jądra
 *
 * Buforuje znaki z klawiatury do końca linii i wykonuje komendę.
 * Shell jest zwykłym wątkiem jądra: przerwanie klawiatury tylko wrzuca
 * znak do bufora kołowego i budzi wątek, a cała reszta - echo, edycja
 * linii i wykonanie komendy - dzieje się już przy włączonych
 * przerwaniach. Dzięki temu komenda może spać, czytać dysk albo czekać
 * na sieć, nie blokując przy tym timera ani reszty systemu.
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

/* Każda konsola wirtualna ma własny shell: własną edytowaną linię,
   własny bufor klawiszy i własny wątek. Bufor musi mieć rozmiar
   będący potęgą dwójki - indeksy liczymy modulo maską. */
#define SHELL_BUF 128

struct shell {
    char line[LINE_MAX];
    int line_len;
    char keybuf[SHELL_BUF];
    uint32_t kb_head, kb_tail;
    struct task *waiter;            /* != NULL tylko gdy wątek śpi */
};

static struct shell shells[CONSOLE_COUNT];

/* Shell konsoli, na której pisze bieżące zadanie */
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

/* Lista komend rosnie, a ekran ma 25 wierszy - dlatego dwie kolumny:
   caly spis miesci sie bez przewijania. */
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

    /* Opcjonalny numer sygnalu za nazwa/pidem; bez niego - SIGKILL */
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

    /* Nazwa zamiast pidu: wygodniejsza, gdy program poszedl z menu
       pulpitu albo gdy numeracja pidow zalezy od kolejnosci startu */
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
    /* Pliki moga miec setki kilobajtow, wiec czytamy kawalkami zamiast
       trzymac caly plik w buforze jadra */
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

/* cp <zrodlo> <cel> - przepisanie pliku na dysk
 *
 * Zrodlo szukamy tam, gdzie szuka go 'run': najpierw w initrd, potem
 * na dysku. Dzieki temu program wgrany razem z jadrem da sie przeniesc
 * na dysk jednym poleceniem - i od tej chwili uruchamiac go stamtad. */
static void cmd_cp(char *args)
{
    char *cel = args;
    while (*cel && *cel != ' ')
        cel++;
    if (*cel == ' ')
        *cel++ = '\0';
    while (*cel == ' ')
        cel++;

    if (!*args || !*cel) {
        printk("uzycie: cp <zrodlo> <cel>\n");
        return;
    }

    uint32_t rozmiar = 0;
    void *wczytany = (void *)0;
    const void *obraz = prog_obraz(args, &rozmiar, &wczytany);
    if (!obraz) {
        printk("cp: nie ma '%s'\n", args);
        return;
    }
    int ok = fs_write(cel, obraz, rozmiar) == 0;
    prog_zwolnij(wczytany);

    if (ok)
        printk("cp: '%s' -> '%s' (%u B)\n", args, cel, rozmiar);
    else
        printk("cp: nie udalo sie zapisac '%s'\n", cel);
}

/* smp - rozdzielenie liczenia na wszystkie rdzenie
 *
 * Kazda jednostka pracy liczy sume kontrolna kawalka ciagu
 * pseudolosowego - roboty na tyle, zeby rdzen mial co robic, i na
 * tyle prostej, zeby wynik dalo sie sprawdzic.
 *
 * Wynik porownujemy z policzonym na jednym rdzeniu. Zgadza sie, gdy
 * jednostki policzono dokladnie raz kazda i nikt nie zadeptal cudzej
 * komorki - a to jest wlasciwe pytanie o wspolbieznosc, nie to, czy
 * cos sie w ogole policzylo.
 */

#define SMP_JEDNOSTEK 64
#define SMP_KROKOW    200000

static uint32_t smp_wyniki[SMP_JEDNOSTEK];

/* Liniowy generator kongruencyjny - zawsze ten sam ciag dla danego
   numeru, wiec wynik nie zalezy od tego, ktory rdzen go policzyl */
static uint32_t smp_licz_kawalek(uint32_t numer)
{
    uint32_t stan = numer * 2654435761u + 12345u;
    uint32_t suma = 0;
    for (uint32_t i = 0; i < SMP_KROKOW; i++) {
        stan = stan * 1103515245u + 12345u;
        suma += stan >> 16;
    }
    return suma;
}

static void smp_jednostka(uint32_t numer, void *dane)
{
    (void)dane;
    if (numer < SMP_JEDNOSTEK)
        smp_wyniki[numer] = smp_licz_kawalek(numer);
}

static void cmd_smp(void)
{
    uint32_t wzorzec = 0;
    for (uint32_t i = 0; i < SMP_JEDNOSTEK; i++)
        wzorzec += smp_licz_kawalek(i);

    for (uint32_t i = 0; i < SMP_JEDNOSTEK; i++)
        smp_wyniki[i] = 0;
    smp_praca(smp_jednostka, 0, SMP_JEDNOSTEK);

    uint32_t razem = 0;
    for (uint32_t i = 0; i < SMP_JEDNOSTEK; i++)
        razem += smp_wyniki[i];

    printk("Rdzeni: %d (obudzonych %d), jednostek %u\n",
           smp_cpus(), smp_started(), (uint32_t)SMP_JEDNOSTEK);
    printk("Udzial:");
    for (int i = 0; i < smp_cpus(); i++)
        printk(" rdzen%d=%u", i, smp_udzial(i));
    printk("\n");
    printk("Wynik: %u, wzorzec: %u - %s\n", razem, wzorzec,
           razem == wzorzec ? "zgadza sie" : "NIE ZGADZA SIE");
    printk("Liczylo rdzeni: %d\n", smp_pracujacych());
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

/* Ping i DHCP czekaja na odpowiedzi (task_sleep). Kiedys shell dzialal
   w kontekscie przerwania i nie wolno mu bylo spac - obie komendy szly
   wiec do osobnych watkow. Teraz shell jest watkiem i po prostu czeka,
   a znaki wpisane w miedzyczasie czekaja w buforze klawiatury. */
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
    else if (!strcmp(cmd, "df"))
        cmd_df();
    else if (!strcmp(cmd, "write"))
        cmd_write(arg);
    else if (!strcmp(cmd, "cp"))
        cmd_cp(arg);
    else if (!strcmp(cmd, "smp"))
        cmd_smp();
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
        *(volatile uint32_t *)0xE0000000 = 1;  /* poza mapowaniem -> #PF */
    else if (!strcmp(cmd, "reboot")) {
        printk("Restart...\n");
        if (!acpi_reboot())
            outb(0x64, 0xFE);  /* zapasowo: impuls przez kontroler klawiatury */
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
    if (c < 0x20)                       /* strzalki i spolka - nie dla shella */
        return;
    if (sh->line_len < LINE_MAX - 1) {
        sh->line[sh->line_len++] = c;
        printk("%c", c);
    }
}

/* Po asynchronicznym wypisie jadra (DHCP, ping w tle) prompt zostaje
   gdzies wyzej na ekranie - odrysuj go razem z niedokonczona linia. */
void shell_refresh(void)
{
    struct shell *sh = my_shell();
    prompt();
    sh->line[sh->line_len] = '\0';
    if (sh->line_len)
        printk("%s", sh->line);
}

/* Kontekst przerwania klawiatury (IF=0): tylko odloz znak i obudz
   watek. Znak przychodzi z konsoli, ktora akurat widac. */
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

/* Kontekst watku: czeka na znak z bufora wlasnej konsoli */
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
    task_set_vc(vc);                    /* od teraz piszemy na swojej konsoli */
    struct shell *sh = &shells[vc];

    /* Pierwsza konsola dostala powitanie jeszcze w shell_init, zeby
       komunikaty startowe i prompt ustawily sie w przewidywalnej
       kolejnosci. Pozostale witaja sie same. */
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
