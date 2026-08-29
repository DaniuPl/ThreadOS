/* Shell jadra 64-bitowego
 *
 * Zwykly watek jadra: czyta znaki z klawiatury, sklada z nich linie
 * i wykonuje polecenia. Nie ma w nim ani przerwan, ani stanu globalnego
 * poza wlasnym buforem linii - to zwykla petla, ktora akurat dziala
 * rownolegle z reszta systemu.
 *
 * Polecen jest tyle, ile jadro naprawde umie. Kazde z nich pokazuje
 * cos, czego nie da sie zobaczyc inaczej niz pytajac system w biegu -
 * i to jest cala roznica miedzy shellem a scenariuszem wypisanym przy
 * starcie.
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
#include "../kernel/include/io.h"
#include "../kernel/include/fs.h"
#include "../kernel/include/fbcon.h"
#include "../kernel/include/net.h"
#include "../kernel/include/video.h"
#include "../kernel/include/desktop.h"
#include "../kernel/include/vga.h"

#define LINE_MAX 80

/* Czy linia zaczyna sie od danego slowa? (strncmp jadro nie ma -
   i nie ma po co miec dla jednego miejsca) */
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
    printk("           rm <plik>, cp <zrodlo> <cel>, uptime,\n");
    printk("           run <program>, clear,\n");
    printk("           ifconfig, arp, ping <adres>, gfx, pulpit,\n");
    printk("           reboot, poweroff\n");
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

    uint32_t wuz, wcal, wout, win;
    swap_stats(&wuz, &wcal, &wout, &win);
    if (wcal)
        printk("Wymiana: %u/%u stron na dysku "
               "(wyrzuconych %u, sciagnietych %u)\n", wuz, wcal, wout, win);
    else
        printk("Wymiana: brak pliku wymiany\n");
}

/* Zapis i odczyt sektora - jedyny sposob, zeby sprawdzic dysk
   naprawde, a nie tylko to, ze kontroler sie przedstawil.
   Ostatni sektor: nic tam jeszcze nie lezy. */
static void cmd_disk(void)
{
    if (!blk_present()) {
        printk("disk: brak dysku\n");
        return;
    }
    printk("Dysk: %s, %u sektorow (%u MiB)\n",
           blk_name(), blk_sectors(), blk_sectors() / 2048);

    static uint8_t bufor[512];
    uint32_t lba = blk_sectors() ? blk_sectors() - 1 : 0;

    for (int i = 0; i < 512; i++)
        bufor[i] = (uint8_t)(i + 7);
    if (blk_write(lba, 1, bufor) < 0) {
        printk("disk: zapis sektora %u nie przeszedl\n", lba);
        return;
    }

    for (int i = 0; i < 512; i++)
        bufor[i] = 0;
    if (blk_read(lba, 1, bufor) < 0) {
        printk("disk: odczyt sektora %u nie przeszedl\n", lba);
        return;
    }

    int rozne = 0;
    for (int i = 0; i < 512; i++)
        if (bufor[i] != (uint8_t)(i + 7))
            rozne++;
    printk("disk: sektor %u zapisany i odczytany, roznic %d\n", lba, rozne);
}

/* --- System plikow ------------------------------------------------
 *
 * Trzy polecenia wystarcza, zeby sprawdzic, czy plik naprawde lezy na
 * dysku, a nie tylko w pamieci: zapisac, wypisac, zobaczyc na liscie.
 * Czwarty dowod przychodzi sam - po ponownym uruchomieniu.
 */
static void cmd_pwd(void)
{
    char sciezka[FS_NAME_MAX * 4];
    fs_getpath(task64_cwd(), sciezka);
    printk("%s\n", sciezka);
}

static void cmd_fls(void)
{
    if (!fs_ready()) {
        printk("fls: brak systemu plikow\n");
        return;
    }

    /* Wypisujemy katalog, w ktorym stoi shell - a nie zawsze glowny */
    char nazwa[FS_NAME_MAX + 1];
    uint32_t rozmiar = 0;
    int katalog = 0;
    cmd_pwd();
    for (int i = 0; fs_readdir(task64_cwd(), i, nazwa,
                               &rozmiar, &katalog) == 1; i++)
        printk("  %s%s  (%u B)\n", nazwa, katalog ? "/" : "", rozmiar);

    uint32_t uzyte = 0, wszystkie = 0;
    fs_usage(&uzyte, &wszystkie);
    printk("Zajetosc: %u/%u blokow po %u B\n", uzyte, wszystkie, FS_BLOCK);
}

static void cmd_cat(const char *nazwa)
{
    static char bufor[512];

    if (!fs_ready()) {
        printk("cat: brak systemu plikow\n");
        return;
    }
    int slot = fs_open(task64_cwd(), nazwa, 0);
    int n = slot < 0 ? -1 : fs_read_at(slot, 0, bufor, sizeof(bufor) - 1);
    if (n < 0) {
        printk("cat: nie ma pliku '%s'\n", nazwa);
        return;
    }
    bufor[n] = '\0';
    printk("%s", bufor);
    if (n && bufor[n - 1] != '\n')
        printk("\n");
}

/* write <plik> <tresc> - reszta linii idzie do pliku */
static void cmd_write(char *reszta)
{
    if (!fs_ready()) {
        printk("write: brak systemu plikow\n");
        return;
    }

    char *tresc = reszta;
    while (*tresc && *tresc != ' ')
        tresc++;
    if (!*tresc) {
        printk("write: podaj nazwe i tresc\n");
        return;
    }
    *tresc++ = '\0';                 /* nazwa konczy sie tu */
    while (*tresc == ' ')
        tresc++;

    uint32_t len = 0;
    while (tresc[len])
        len++;
    tresc[len++] = '\n';             /* plik tekstowy konczy sie linia */

    int slot = fs_open(task64_cwd(), reszta, 1);
    if (slot < 0 || fs_truncate(slot) < 0 ||
        fs_write_at(slot, 0, tresc, len) < 0)
        printk("write: nie udalo sie zapisac '%s'\n", reszta);
    else
        printk("write: '%s' zapisany (%u B)\n", reszta, len);
}

/* cp <zrodlo> <cel> - przepisanie pliku na dysk
 *
 * Zrodlo szukamy tam, gdzie szuka go 'run': najpierw w initrd, potem
 * na dysku. Dzieki temu program wgrany razem z jadrem da sie przeniesc
 * na dysk jednym poleceniem - i od tej chwili uruchamiac go stamtad,
 * takze wtedy, gdy w initrd go juz nie ma. */
static void cmd_cp(char *reszta)
{
    if (!fs_ready()) {
        printk("cp: brak systemu plikow\n");
        return;
    }

    char *cel = reszta;
    while (*cel && *cel != ' ')
        cel++;
    if (!*cel) {
        printk("cp: podaj zrodlo i cel, np. 'cp hello64 hello64'\n");
        return;
    }
    *cel++ = '\0';                    /* nazwa zrodla konczy sie tu */
    while (*cel == ' ')
        cel++;
    if (!*cel) {
        printk("cp: podaj cel\n");
        return;
    }

    uint32_t rozmiar = 0;
    void *wczytany = (void *)0;
    const void *obraz = prog_obraz(reszta, &rozmiar, &wczytany);
    if (!obraz) {
        printk("cp: nie ma '%s'\n", reszta);
        return;
    }

    /* fs_write_at oddaje LICZBE zapisanych bajtow, nie zero - plik
       skrocony w polowie tez trzeba uznac za bledny */
    int slot = fs_open(task64_cwd(), cel, 1);
    int ok = slot >= 0 && fs_truncate(slot) >= 0 &&
             fs_write_at(slot, 0, obraz, rozmiar) == (int)rozmiar;
    prog_zwolnij(wczytany);

    if (ok)
        printk("cp: '%s' -> '%s' (%u B)\n", reszta, cel, rozmiar);
    else
        printk("cp: nie udalo sie zapisac '%s'\n", cel);
}

/* Rysunek sprawdzajacy tryb graficzny.
 *
 * Cztery prostokaty w rogach, ramka i napis - tyle wystarczy, zeby
 * bylo widac, czy kolory trafiaja tam, gdzie trzeba, i czy piksel
 * o wspolrzednych (x, y) laduje pod (x, y). Zle policzony bajt na
 * piksel widac tu od razu: obraz rozjezdza sie w skos.
 */
static void cmd_gfx(void)
{
    if (!video_w()) {
        printk("gfx: brak trybu graficznego\n");
        return;
    }
    int w = video_w(), h = video_h();

    /* Komunikat PRZED rysunkiem: konsola i rysunek dziela ten sam
       bufor, a kazdy wypis przerysowuje ja w calosci - wiec wszystko,
       co napisane po rysunku, wchodzi na niego. */
    printk("gfx: rysuje %ux%u, %u bitow na piksel\n",
           (uint32_t)w, (uint32_t)h, (uint32_t)fbcon_bpp());

    /* Paleta video.c ma zdefiniowane indeksy 0-5, a nie szesnascie
       kolorow trybu tekstowego: 1 czerwony, 2 zielony, 3 niebieski,
       4 bialy, 5 zolty. */
    video_clear(3);                                    /* niebieskie tlo */
    video_fill_rect(0, 0, w / 4, h / 4, 1);            /* lewy gorny  */
    video_fill_rect(w - w / 4, 0, w / 4, h / 4, 2);    /* prawy gorny */
    video_fill_rect(0, h - h / 4, w / 4, h / 4, 5);    /* lewy dolny  */
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

static void cmd_uptime(void)
{
    uint32_t t = pit_ticks();
    printk("Uptime: %u s (%u tyknac po 10 ms)\n", t / 100, t);
}

/* Argument polecenia: pierwsze slowo za nazwa, bez spacji przed nim */
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
        int nowy = FS_ROOT;
        if (!kat)
            printk("cd: podaj katalog\n");
        else if (fs_chdir(task64_cwd(), kat, &nowy) < 0)
            printk("cd: nie ma katalogu '%s'\n", kat);
        else
            task64_set_cwd(nowy);
    } else if (starts_with(line, "mkdir ")) {
        const char *kat = argument(line, 6);
        if (!kat)
            printk("mkdir: podaj nazwe\n");
        else if (fs_mkdir(task64_cwd(), kat) < 0)
            printk("mkdir: nie udalo sie zalozyc '%s'\n", kat);
        else
            printk("mkdir: '%s' zalozony\n", kat);
    } else if (starts_with(line, "rm ")) {
        const char *plik = argument(line, 3);
        if (!plik)
            printk("rm: podaj nazwe\n");
        else if (fs_unlink(task64_cwd(), plik) < 0)
            printk("rm: nie ma '%s'\n", plik);
        else
            printk("rm: '%s' usuniety\n", plik);
    } else if (starts_with(line, "cat ")) {
        const char *nazwa = argument(line, 4);
        if (nazwa)
            cmd_cat(nazwa);
        else
            printk("cat: podaj nazwe pliku\n");
    } else if (starts_with(line, "cp ")) {
        char *arg = (char *)argument(line, 3);
        if (!arg)
            printk("cp: podaj zrodlo i cel\n");
        else
            cmd_cp(arg);
    } else if (starts_with(line, "write ")) {
        char *reszta = (char *)argument(line, 6);
        if (reszta)
            cmd_write(reszta);
        else
            printk("write: podaj nazwe i tresc\n");
    } else if (!strcmp(line, "pulpit")) {
        /* Przejscie w tryb graficzny jest jednokierunkowe: od tej
           chwili klawiature przejmuje pulpit, a konsola tekstowa
           przestaje byc widoczna */
        printk("Pulpit startuje - konsola znika\n");
        desktop_start();
    } else if (!strcmp(line, "gfx")) {
        cmd_gfx();
    } else if (!strcmp(line, "ifconfig")) {
        net_ifconfig();
    } else if (!strcmp(line, "arp")) {
        net_arp_dump();
    } else if (starts_with(line, "ping ")) {
        const char *adres = argument(line, 5);
        uint32_t ip = 0;
        if (!adres)
            printk("ping: podaj adres\n");
        else if (!net_parse_ip(adres, &ip))
            printk("ping: '%s' to nie jest adres IP\n", adres);
        else
            net_ping(ip, 3);
    } else if (!strcmp(line, "disk")) {
        cmd_disk();
    } else if (!strcmp(line, "uptime")) {
        cmd_uptime();
    } else if (!strcmp(line, "clear")) {
        vga_clear();
    } else if (!strcmp(line, "reboot")) {
        printk("Restart...\n");
        /* Gdy tablice ACPI nie maja rejestru resetu, zostaje sposob
           starszy od nich: impuls przez kontroler klawiatury. */
        if (!acpi_reboot())
            outb(0x64, 0xFE);
    } else if (!strcmp(line, "poweroff")) {
        printk("Wylaczanie zasilania...\n");
        acpi_poweroff();
        printk("poweroff: nie udalo sie - "
               "mozna wylaczyc zasilanie recznie\n");
    } else if (starts_with(line, "run ")) {
        const char *name = argument(line, 4);
        if (!name) {
            printk("run: podaj nazwe programu (zobacz 'ls')\n");
            return;
        }
        /* Nazwa to pierwsze slowo, reszta to argumenty */
        static char prog[16];
        int pn = 0;
        while (name[pn] && name[pn] != ' ' && pn < (int)sizeof(prog) - 1) {
            prog[pn] = name[pn];
            pn++;
        }
        prog[pn] = '\0';
        name = prog;

        if (!prog_istnieje(name)) {
            printk("run: nie ma programu '%s' "
                   "(zobacz 'ls' i 'fls')\n", name);
            return;
        }
        /* Reszta linii to argumenty programu - shell tylko dzieli
           ja na slowa, a co z nich wyniknie, jest juz sprawa programu */
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

        uint32_t ramki_przed = pmm_used_frames();
        uint32_t strony_przed = isr64_heap_pages();
        int id = proc64_spawn(name, args.argc ? &args : 0, 0);
        if (id < 0) {
            printk("run: nie udalo sie uruchomic '%s'\n", name);
            return;
        }
        /* Shell czeka na swoje dziecko - tak samo jak zrobilby to
           program przez wywolanie systemowe. Przez ten czas nie
           odpowiada, i tak ma byc. */
        keyboard64_set_intr_target(id);   /* od teraz Ctrl+C idzie do niego */
        int code = 0;
        int done = task64_wait(&code);
        keyboard64_set_intr_target(-1);
        if (done < 0)
            printk("run: dziecko przepadlo\n");
        else
            printk("[%s] koniec, kod wyjscia %d (ramek %u -> %u, "
                   "stron sterty %u)\n",
                   name, code, ramki_przed, pmm_used_frames(),
                   isr64_heap_pages() - strony_przed);
    } else {
        printk("Nieznane polecenie: %s (sprobuj 'help')\n", line);
    }
}

static void shell_thread(void *arg)
{
    int vc = (int)(long)arg;
    task64_set_vc(vc);          /* od teraz piszemy na swojej konsoli */

    /* Wiersz polecenia jest wlasnoscia watku, nie jadra: kazda konsola
       ma wlasny shell, wiec kazdy ma i wlasny bufor - na wlasnym stosie,
       bez tablic indeksowanych numerem konsoli. */
    char line[LINE_MAX];
    int line_len = 0;

    /* Pierwsza konsola dostaje pelne powitanie ze spisem polecen,
       pozostale krotka notke: cztery spisy pod rzad na porcie
       szeregowym nikomu nie sa potrzebne. */
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
        /* Klawisze sterujace (strzalki, Ctrl+C) shell na razie mija */
    }
}

void shell64_start(void)
{
    for (int vc = 0; vc < CONSOLE_COUNT; vc++)
        task64_create("shell", shell_thread, (void *)(long)vc);
}
