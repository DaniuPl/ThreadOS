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

/* --- System plikow ------------------------------------------------
 *
 * Trzy polecenia wystarcza, zeby sprawdzic, czy plik naprawde lezy na
 * dysku, a nie tylko w pamieci: zapisac, wypisac, zobaczyc na liscie.
 * Czwarty dowod przychodzi sam - po ponownym uruchomieniu.
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

    /* Wypisujemy katalog, w ktorym stoi shell - a nie zawsze glowny */
    char name[FS_NAME_MAX + 1];
    uint32_t size = 0;
    int dir = 0;
    cmd_pwd();
    for (int i = 0; fs_readdir(task64_cwd(), i, name,
                               &size, &dir) == 1; i++)
        printk("  %s%s  (%u B)\n", name, dir ? "/" : "", size);

    uint32_t uzyte = 0, wszystkie = 0;
    fs_usage(&uzyte, &wszystkie);
    printk("Zajetosc: %u/%u blokow po %u B\n", uzyte, wszystkie, FS_BLOCK);
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

/* write <plik> <tresc> - reszta linii idzie do pliku */
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
    *body++ = '\0';                 /* nazwa konczy sie tu */
    while (*body == ' ')
        body++;

    uint32_t len = 0;
    while (body[len])
        len++;
    body[len++] = '\n';             /* plik tekstowy konczy sie linia */

    int slot = fs_open(task64_cwd(), rest, 1);
    if (slot < 0 || fs_truncate(slot) < 0 ||
        fs_write_at(slot, 0, body, len) < 0)
        printk("write: nie udalo sie zapisac '%s'\n", rest);
    else
        printk("write: '%s' zapisany (%u B)\n", rest, len);
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

#define SMP_UNITS 64
#define SMP_STEPS    200000

static uint32_t smp_results[SMP_UNITS];

/* Liniowy generator kongruencyjny - zawsze ten sam ciag dla danego
   numeru, wiec wynik nie zalezy od tego, ktory rdzen go policzyl */
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

    /* Wlasny zegar kazdego rdzenia. Rdzen rozruchowy chodzi z PIT
       i tu ma zero - wtorne licza swoje tykniecia w lokalnym APIC,
       i to jest jedyny dowod na to, ze w ogole dostaja przerwania. */
    printk("Zegary rdzeni:");
    for (int i = 0; i < smp_cpus(); i++)
        printk(" rdzen%d=%u", i, smp_ticks(i));
    printk("\n");
}

/* cp <zrodlo> <cel> - przepisanie pliku na dysk
 *
 * Zrodlo szukamy tam, gdzie szuka go 'run': najpierw w initrd, potem
 * na dysku. Dzieki temu program wgrany razem z jadrem da sie przeniesc
 * na dysk jednym poleceniem - i od tej chwili uruchamiac go stamtad,
 * takze wtedy, gdy w initrd go juz nie ma. */
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
    *dst++ = '\0';                    /* nazwa zrodla konczy sie tu */
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

    /* fs_write_at oddaje LICZBE zapisanych bajtow, nie zero - plik
       skrocony w polowie tez trzeba uznac za bledny */
    int slot = fs_open(task64_cwd(), dst, 1);
    int ok = slot >= 0 && fs_truncate(slot) >= 0 &&
             fs_write_at(slot, 0, image, size) == (int)size;
    prog_release(loaded);

    if (ok)
        printk("cp: '%s' -> '%s' (%u B)\n", rest, dst, size);
    else
        printk("cp: nie udalo sie zapisac '%s'\n", dst);
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

/* date - godzina z zegara plyty glownej */
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
    } else if (starts_with(line, "dns ")) {
        /* Reczna zmiana serwera nazw - DHCP podaje swoj, ale nie
           zawsze ten jest tym, o ktory chodzi */
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
        /* Gdy tablice ACPI nie maja rejestru resetu, zostaje sposob
           starszy od nich: impuls przez kontroler klawiatury. */
        if (!acpi_reboot())
            outb(0x64, 0xFE);
    } else if (!strcmp(line, "poweroff")) {
        printk("Wylaczanie zasilania...\n");
        acpi_poweroff();
        printk("poweroff: nie udalo sie - "
               "mozna wylaczyc zasilanie recznie\n");
    } else if (starts_with(line, "runw ")) {
        /* Jak 'run', ale programowi wolno biec na kazdym rdzeniu -
           i nie czekamy na niego, zeby dalo sie puscic kilka naraz.
           Jego strona jadrowa i tak chodzi pod wielkim zamkiem;
           rownolegle idzie liczenie w pierscieniu 3. */
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
        /* Nazwa to pierwsze slowo, reszta to argumenty */
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

        uint32_t frames_before = pmm_used_frames();
        uint32_t pages_before = isr64_heap_pages();
        uint32_t stack_before = isr64_stack_pages();
        uint32_t files_before = isr64_mmap_pages();
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
