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
#include "include/io.h"

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

static void cmd_uptime(void)
{
    uint32_t s = pit_ticks() / 100;
    printk("uptime: %u s (%u tyknac timera po 10 ms)\n", s, pit_ticks());
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

void shell_init(void)
{
    keyboard_set_handler(shell_on_char);
    printk("\nProtonOS shell - wpisz 'help', by zobaczyc komendy.\n");
    prompt();
}
