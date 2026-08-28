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
#include "syscall64.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/string.h"
#include "../kernel/include/pmm.h"
#include "../kernel/include/pit.h"
#include "../kernel/include/initrd.h"
#include "../kernel/include/blk.h"
#include "../kernel/include/fs.h"
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

static char line[LINE_MAX];
static int line_len;

static void prompt(void)
{
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    printk("proton64> ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_help(void)
{
    printk("Polecenia: help, ps, mem, ls, disk, fls, cat <plik>,\n");
    printk("           write <plik> <tresc>, uptime, run <program>, "
           "clear\n");
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
static void cmd_fls(void)
{
    if (!fs_ready()) {
        printk("fls: brak systemu plikow\n");
        return;
    }
    fs_list();

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
    int n = fs_read(nazwa, bufor, sizeof(bufor) - 1);
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

    if (fs_write(reszta, tresc, len) < 0)
        printk("write: nie udalo sie zapisac '%s'\n", reszta);
    else
        printk("write: '%s' zapisany (%u B)\n", reszta, len);
}

static void cmd_uptime(void)
{
    uint32_t t = pit_ticks();
    printk("Uptime: %u s (%u tyknac po 10 ms)\n", t / 100, t);
}

/* Argument polecenia: pierwsze slowo za nazwa, bez spacji przed nim */
static const char *argument(int cmd_len)
{
    int i = cmd_len;
    while (line[i] == ' ')
        i++;
    return line[i] ? &line[i] : (const char *)0;
}

static void execute(void)
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
    } else if (starts_with(line, "cat ")) {
        const char *nazwa = argument(4);
        if (nazwa)
            cmd_cat(nazwa);
        else
            printk("cat: podaj nazwe pliku\n");
    } else if (starts_with(line, "write ")) {
        char *reszta = (char *)argument(6);
        if (reszta)
            cmd_write(reszta);
        else
            printk("write: podaj nazwe i tresc\n");
    } else if (!strcmp(line, "disk")) {
        cmd_disk();
    } else if (!strcmp(line, "uptime")) {
        cmd_uptime();
    } else if (!strcmp(line, "clear")) {
        vga_clear();
    } else if (starts_with(line, "run ")) {
        const char *name = argument(4);
        if (!name) {
            printk("run: podaj nazwe programu (zobacz 'ls')\n");
            return;
        }
        uint32_t size = 0;
        if (!initrd_find(name, &size)) {
            printk("run: nie ma programu '%s'\n", name);
            return;
        }
        int id = proc64_spawn(name, 0, 0);
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
            printk("[%s] koniec, kod wyjscia %d\n", name, code);
    } else {
        printk("Nieznane polecenie: %s (sprobuj 'help')\n", line);
    }
}

static void shell_thread(void *arg)
{
    (void)arg;

    printk("\n");
    cmd_help();
    prompt();

    for (;;) {
        char c = keyboard64_getchar();

        if (c == '\n') {
            printk("\n");
            line[line_len] = '\0';
            execute();
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
    task64_create("shell", shell_thread, 0);
}
