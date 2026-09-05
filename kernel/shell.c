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
#include "include/tlb.h"
#include "include/isr.h"
#include "include/config.h"
#include "include/user.h"
#include "include/clip.h"
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
#include "include/pci.h"
#include "include/part.h"
#include "include/fatcmd.h"
#include "include/install.h"
#include "include/blk.h"
#include "include/usbhc.h"
#include "include/usbkbd.h"
#include "include/usbmouse.h"
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

/* The prompt says the machine's name and wears the colour it was
   given - both come from the settings, so both survive a restart */
static void prompt(void)
{
    vga_set_color((enum vga_color)(config_get_num("kolor", VGA_LIGHT_CYAN) & 15),
                  VGA_BLACK);
    /* '#' for an admin, '>' for everybody else - the oldest way of
       saying it, and the one that is seen without being read. */
    printk("%s%c ", config_get("nazwa"),
           user_shell_level() == USER_ADMIN ? '#' : '>');
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* The command list grows and the screen has 25 rows - hence two
   columns. Two columns of forty commands is twenty rows, which fits
   only if nothing is above it, so the listing starts by clearing the
   screen: a reference one has to scroll to read is not a reference,
   and whatever was above it was the boot log or the last command,
   neither of which anybody is reading at the moment they ask what the
   commands are. */
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
    {"mv A B",   "zmien nazwe pliku"},
    {"set",      "ustawienia systemu"},
    {"kim",      "czy jestes adminem"},
    {"sudo",     "komenda jako admin"},
    {"wyloguj",  "przestan byc adminem"},
    {"haslo",    "zmien haslo admina"},
    {"smp",      "licz na kazdym rdzeniu"},
    {"smpmem",   "proba zamkow na pamieci"},
    {"smpthreads", "watki jadra na rdzeniach"},
    {"runw",     "run na dowolnym rdzeniu"},
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
    {"kill P S", "sygnal do procesu"},
    {"clear",    "wyczysc ekran"},
    {"ekran",    "tryb ekranu, koszt klatki"},
    {"sprzet",   "co jest w tej maszynie"},
    {"instaluj", "ProtonOS na dysk (ekran)"},
    {"fat [N]",  "partycja EFI (FAT)"},
    {"fatls P",  "katalog na FAT"},
    {"fatcat P", "plik z FAT"},
    {"fatcopy",  "z initrd na FAT"},
    {"gfx",      "test trybu 13h"},
    {"vesa",     "test trybu VESA"},
    {"crash",    "test page faulta"},
    {"reboot",   "restart komputera"},
    {"poweroff", "wylacz komputer"},
};

#define COMMAND_N ((int)(sizeof(commands) / sizeof(commands[0])))

/* A column, and never wider than one.
 *
 * This only padded before, which works right up to the first entry
 * longer than the column - and then that row is wider than the screen,
 * the terminal wraps it, and the listing that was meant to fit in
 * twenty rows takes twenty-five and scrolls its own heading away. A
 * column has a width or it is not a column. */
static void print_padded(const char *s, int width)
{
    int i = 0;
    for (; s[i] && (!width || i < width); i++)
        printk("%c", s[i]);
    for (; i < width; i++)
        printk(" ");
}

static void cmd_help(void)
{
    int rows = (COMMAND_N + 1) / 2;
    vga_clear();
    printk("Dostepne komendy:\n");
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < 2; c++) {
            int i = r + c * rows;
            if (i >= COMMAND_N)
                break;
            printk("  ");
            print_padded(commands[i].name, 11);
            print_padded(commands[i].desc, c ? 0 : 24);
        }
        printk("\n");
    }
    printk("Konsole: Alt+F1..F%d (teraz %d)\n",
           CONSOLE_COUNT, task_vc() + 1);
}

/* What the screen is and how much of it the last frame actually cost.
   The compositor redraws everything thirty times a second; the number
   that matters is how much of it had to cross the bus to the card. */
static void cmd_ekran(void)
{
    printk("tryb:         %dx%d, %s, skala %d\n", video_w(), video_h(),
           video_true_colour() ? "kolory" : "paleta 256", video_scale());
    printk("ostatnia klatka: %u wierszy na karte (z %d)\n",
           video_present_rows(), video_h());
}

/* What this machine actually is.
 *
 * Written for a first boot on hardware nobody has tried yet. Every
 * line here answers a question one asks while standing in front of a
 * laptop that will not do something: what did the loader give us for
 * a screen, is there anything on the PS/2 ports, did the USB bus have
 * a keyboard or a pointer on it, and - when the answer to the pointer
 * is no - what else is in the machine that it might be hiding behind.
 *
 * The PCI listing is the last of those. A touchpad that is not on the
 * PS/2 port is usually on an I2C controller, and an I2C controller is
 * a device of the serial-bus class sitting in plain sight here. We do
 * not speak that bus; being able to SAY that is the difference between
 * a mystery and a known gap. */
static const char *pci_class_name(uint32_t class)
{
    switch (class >> 24) {
    case 0x01: return "dysk";
    case 0x02: return "siec";
    case 0x03: return "grafika";
    case 0x04: return "dzwiek/wideo";
    case 0x06: return "mostek";
    case 0x08: return "kontroler systemowy";
    case 0x09: return "wejscie (mysz, tablet)";
    case 0x0C: return ((class >> 16) & 0xFF) == 0x03 ? "USB" : "magistrala szeregowa";
    case 0x11: return "przetwarzanie sygnalu";
    default:   return "inne";
    }
}

static void cmd_sprzet(void)
{
    printk("ekran:    %dx%d, %s, skala %d, bufor %s\n",
           video_w(), video_h(),
           video_true_colour() ? "kolory" : "paleta 256", video_scale(),
           video_have_loader_lfb() ? "od loadera" : "ustawiony przez jadro");

    if (!ps2_controller_present())
        printk("PS/2:     BRAK kontrolera 8042 - ani klawiatury, ani "
               "myszy na tych portach\n");
    else
        printk("PS/2:     kontroler 8042 jest, mysz %s\n",
               mouse_present() ? (mouse_has_wheel() ? "tak (z kolkiem)"
                                                    : "tak (bez kolka)")
                               : "NIE");
    printk("USB:      %s, klawiatura %s, mysz %s\n",
           usbhc_name(), usbkbd_present() ? "tak" : "nie",
           usbmouse_present() ? "tak" : "nie");

    /* The disk, and what is already on it. A machine booted from a
       stick sees the laptop's own disk here - and before anyone
       proposes to write to it, it is worth reading what would be
       written over. */
    printk("dysk:     %s, %u sektorow po 512 B (%u MiB)\n", blk_name(),
           blk_sectors(), blk_sectors() / 2048);
    int np = part_scan();
    if (np < 0)
        printk("          brak dysku\n");
    else if (np == 0)
        printk("          bez tablicy partycji - caly dysk jest nasz\n");
    else {
        printk("          tablica %s, partycji: %u\n",
               part_is_gpt() ? "GPT" : "MBR", (uint32_t)np);
        for (int i = 0; i < np; i++) {
            struct partition pt;
            if (!part_get(i, &pt))
                break;
            printk("          %u: od %u, %u MiB, %s%s%s\n",
                   (uint32_t)(i + 1), (uint32_t)pt.start,
                   (uint32_t)(pt.sectors / 2048), pt.kind,
                   pt.label[0] ? " " : "", pt.label);
        }
    }

    /* And what else is in here, so that a gap has a name */
    int serial_bus = 0;
    printk("PCI:\n");
    for (int i = 0; ; i++) {
        struct pci_dev d;
        uint32_t class;
        if (!pci_enumerate(i, &d, &class))
            break;
        uint8_t base = (uint8_t)(class >> 24);
        uint8_t sub = (uint8_t)((class >> 16) & 0xFF);
        if ((base == 0x0C && sub != 0x03) || base == 0x11)
            serial_bus++;
        /* printk has no field widths, so the columns are spelled out
           rather than padded - and a slot number is more readable in
           decimal here anyway. */
        printk("  %u.%u  %x:%x  klasa %x.%x  %s\n",
               (uint32_t)d.slot, (uint32_t)d.func,
               (uint32_t)d.vendor, (uint32_t)d.device,
               (uint32_t)base, (uint32_t)sub, pci_class_name(class));
    }

    if (!mouse_present() && !usbmouse_present() && serial_bus)
        printk("Wskaznika nie ma na zadnej znanej drodze, a w maszynie\n"
               "jest %d kontroler(ow) magistrali szeregowej - touchpad\n"
               "siedzi zapewne na I2C (HID przez I2C), ktorego nie umiemy.\n"
               "Mysz w gniazdo USB zadziala.\n", serial_bus);
}

/* Reads a line back with what was typed visible - the confirmation
   below has to be READ by the person typing it. Defined next to
   read_secret, which is the same loop with stars. */
static void read_word(char *out, int max);

/* Formatting, and the asking that has to come before it.
 *
 * The startup path will not touch a disk that has anything on it, so
 * this is the only way a disk with a Windows on it becomes a disk with
 * PFS on it - and it is deliberately a thing one has to mean. The
 * confirmation is the word, typed out: a key to press is something a
 * person does by reflex, and a word is something they have to decide
 * to type. */
static void cmd_formatuj(char *arg)
{
    if (!blk_present()) {
        printk("formatuj: nie ma dysku\n");
        return;
    }

    int which = 0;                 /* 0 = the whole disk */
    for (const char *p = arg; *p >= '0' && *p <= '9'; p++)
        which = which * 10 + (*p - '0');

    printk("Dysk: %s, %u MiB\n", blk_name(), blk_sectors() / 2048);
    int np = part_scan();
    if (np > 0) {
        printk("Sa na nim %u partycje(i) - tablica %s:\n", (uint32_t)np,
               part_is_gpt() ? "GPT" : "MBR");
        for (int i = 0; i < np; i++) {
            struct partition pt;
            if (!part_get(i, &pt))
                break;
            printk("  %u: %u MiB, %s %s\n", (uint32_t)(i + 1),
                   (uint32_t)(pt.sectors / 2048), pt.kind, pt.label);
        }
    }

    if (which > 0) {
        struct partition pt;
        if (which > np || !part_get(which - 1, &pt)) {
            printk("formatuj: nie ma partycji %u\n", (uint32_t)which);
            return;
        }
        printk("\nTO SKASUJE PARTYCJE %u (%u MiB, %s %s).\n",
               (uint32_t)which, (uint32_t)(pt.sectors / 2048), pt.kind,
               pt.label);
        printk("Reszta dysku i tablica partycji zostaja nietkniete.\n");
    } else if (np > 0) {
        printk("\nTO SKASUJE CALY DYSK - tablice partycji i WSZYSTKIE "
               "%u partycje(i) powyzej.\n", (uint32_t)np);
        printk("Zeby zajac tylko jedna, napisz 'formatuj N'.\n");
    } else {
        printk("\nTO SKASUJE CALY DYSK, ze wszystkim, co na nim jest.\n");
    }
    printk("Wpisz TAK (duzymi literami), zeby to zrobic: ");

    char answer[8];
    read_word(answer, (int)sizeof(answer));

    if (strcmp(answer, "TAK") != 0) {
        printk("formatuj: nie skasowano niczego\n");
        return;
    }
    if (fs_format_now(which))
        printk("formatuj: gotowe - %s jest teraz PFS-em ProtonOS-a\n",
               which ? "ta partycja" : "caly dysk");
    else
        printk("formatuj: nie udalo sie\n");
}

/* Installing onto the machine's own disk.
 *
 * Reading and printing first, then a person saying yes, and only then
 * writing - the same shape as formatting, and for the same reason:
 * what is on the other side of this is somebody's laptop. */
/* Does the line start with this word? */
static int starts(const char *s, const char *word)
{
    while (*word)
        if (*s++ != *word++)
            return 0;
    return *s == '\0' || *s == ' ';
}

/* 'instaluj' takes the whole screen; 'instaluj tekstem' says the same
   things here, which is what one wants when there is no screen to draw
   on - or when the answer has to end up in a log. */
static void cmd_install(const char *arg)
{
    char answer[8];
    int mode = INST_ALONGSIDE, disk = -1;

    if (!arg || !arg[0]) {
        if (install_graphical())
            return;
        printk("instaluj: pulpit juz dziala, a instalator potrzebuje "
               "calego ekranu.\n");
        printk("          Zrestartuj maszyne albo napisz "
               "'instaluj tekstem'.\n");
        return;
    }
    /* "tekstem", then optionally "caly", then optionally a disk
       number counted from one. A machine with several disks is a
       machine where saying which one is the whole question. */
    const char *p = arg;
    if (!starts(p, "tekstem")) {
        printk("uzycie: instaluj [tekstem [caly] [numer dysku]]\n");
        return;
    }
    p += 7;
    while (*p == ' ')
        p++;
    if (starts(p, "caly")) {
        mode = INST_WHOLE;
        p += 4;
        while (*p == ' ')
            p++;
    }
    if (*p >= '1' && *p <= '9') {
        int n = 0;
        while (*p >= '0' && *p <= '9')
            n = n * 10 + (*p++ - '0');
        disk = n - 1;
    }
    if (blk_count() > 1)
        for (int i = 0; i < blk_count(); i++)
            printk("  dysk %u: %s, %u MiB%s\n", (uint32_t)(i + 1),
                   blk_name_of(i), blk_sectors_of(i) / 2048,
                   i == (disk < 0 ? blk_current() : disk) ? "  <-" : "");
    if (!install_plan(mode, disk))
        return;
    /* A different word for the way that destroys things. TAK is what
       one says to a question; KASUJ is what one says to this. */
    const char *word = mode == INST_WHOLE ? "KASUJ" : "TAK";
    printk("\nWpisz %s (duzymi literami), zeby to zrobic: ", word);
    read_word(answer, (int)sizeof(answer));
    if (strcmp(answer, word) != 0) {
        printk("instaluj: nie zapisano niczego\n");
        return;
    }
    install_do();
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
    /* Whether the device agreed to the fourth byte. On a laptop that
       is the difference between two fingers scrolling and two fingers
       doing nothing. */
    printk("mysz: x=%u y=%u przyciski=%u kolko=%s\n", (uint32_t)x,
           (uint32_t)y, (uint32_t)b, mouse_has_wheel() ? "tak" : "nie");
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

/* The system's own files: reading them is nobody's business but
   writing to them is changing the system. The shell asks the same
   question the syscalls ask - otherwise the console would be the way
   round every rule the rest of the machine keeps. */
static int may_touch(const char *name)
{
    if (!user_is_system_file(name) || user_shell_level() == USER_ADMIN)
        return 1;
    printk("brak uprawnien do '%s' - sprobuj przez sudo\n", name);
    return 0;
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
    if (!may_touch(args))
        return;
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
/* mv - a new name for something that exists. Two words in, one entry
   changed on the disk: the file's blocks do not move, which is the
   whole difference between this and 'cp' followed by 'rm'. */
static void cmd_mv(char *args)
{
    char *dst = args;
    while (*dst && *dst != ' ')
        dst++;
    if (*dst == ' ')
        *dst++ = '\0';
    while (*dst == ' ')
        dst++;

    if (!*args || !*dst) {
        printk("uzycie: mv <zrodlo> <cel>\n");
        return;
    }
    if (!may_touch(args) || !may_touch(dst))
        return;
    /* The kernel's shell stands in the root - it is the ring 3 shell
       that walks about with a working directory. */
    if (fs_rename(FS_ROOT, args, dst) == 0)
        printk("mv: '%s' -> '%s'\n", args, dst);
    else
        printk("mv: nie da sie (nie ma zrodla albo nazwa zajeta)\n");
}

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

    if (!may_touch(dst))
        return;

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

    /* What the changed mappings cost. 'lokalnie' are the ones nobody
       else was holding - no interrupt, no waiting; 'zdalnie' are the
       ones that had to reach another core, and 'odpowiedzi' is how many
       cores answered. */
    printk("TLB: lokalnie %u, zdalnie %u, odpowiedzi %u\n",
           tlb_local_only(), tlb_shootdowns(), tlb_remote_acks());
    /* The commonest way into the kernel there is. 'bez zamka' are the
       faults served before the big kernel lock was taken at all. */
    printk("Bledy stron: bez zamka %u, z zamkiem %u\n",
           isr_faults_nolock(), isr_faults_locked());
}

static void cmd_dhcp(void)
{
    if (!dhcp_configure())
        printk("dhcp: brak odpowiedzi serwera\n");
}

/* set - what the machine remembers about itself
 *
 * Without arguments it shows everything, with a name and a value it
 * changes one and writes them all back to the disk. The proof that it
 * worked is not the line it prints but the next boot. */
static void cmd_set(const char *arg)
{
    if (!*arg) {
        printk("Ustawienia (plik %s):\n", CONFIG_FILE);
        for (int i = 0; i < config_count(); i++)
            printk("  %s = %s   (%s)\n", config_name(i), config_value(i),
                   config_help(i));
        printk("uzycie: set <nazwa> <wartosc>\n");
        return;
    }

    /* The name is the first word, the value is what follows it */
    char name[32];
    int i = 0;
    while (arg[i] && arg[i] != ' ' && i < (int)sizeof(name) - 1) {
        name[i] = arg[i];
        i++;
    }
    name[i] = 0;
    while (arg[i] == ' ')
        i++;
    if (!arg[i]) {
        const char *v = config_get(name);
        if (v)
            printk("%s = %s\n", name, v);
        else
            printk("set: nie ma ustawienia '%s'\n", name);
        return;
    }

    if (user_shell_level() != USER_ADMIN) {
        printk("set: zmiana ustawien wymaga admina - 'sudo set %s ...'\n",
               name);
        return;
    }
    if (config_is_hidden(name)) {
        printk("set: '%s' nie zmienia sie tak - zobacz komende 'haslo'\n",
               name);
        return;
    }
    if (config_set(name, arg + i) < 0) {
        printk("set: nie udalo sie zapisac '%s' (zla nazwa, za dluga "
               "wartosc albo brak dysku)\n", name);
        return;
    }
    printk("%s = %s (zapisane)\n", name, config_get(name));

    /* Some settings are read the moment they are needed and some were
       handed to a subsystem once, at boot - those have to be handed
       over again */
    if (!strcmp(name, "dns")) {
        uint32_t ip = config_get_num("dns", 0);
        if (ip)
            net_set_dns(ip);
    }
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

/* --- who is at the machine ------------------------------------------
 *
 * The shell reads its characters one at a time from its own console,
 * in its own thread, so a password can simply be read here: a blocking
 * read inside the command, with stars instead of what was typed. There
 * is nothing asynchronous about it and nothing to remember afterwards.
 */
static char shell_getchar(struct shell *sh);

static void read_word(char *out, int max)
{
    struct shell *sh = my_shell();
    int n = 0;
    for (;;) {
        char c = shell_getchar(sh);
        if (c == '\n')
            break;
        if (c == '\b') {
            if (n) {
                n--;
                printk("\b \b");
            }
            continue;
        }
        if (c < 0x20)
            continue;
        if (n < max - 1) {
            out[n++] = c;
            printk("%c", c);
        }
    }
    out[n] = 0;
    printk("\n");
}

static void read_secret(char *out, int max)
{
    struct shell *sh = my_shell();
    int n = 0;
    for (;;) {
        char c = shell_getchar(sh);
        if (c == '\n')
            break;
        if (c == '\b') {
            if (n) {
                n--;
                printk("\b");
            }
            continue;
        }
        if (c < 0x20)
            continue;
        if (n < max - 1) {
            out[n++] = c;
            printk("*");           /* something is seen; what, is not */
        }
    }
    out[n] = 0;
    printk("\n");
}

static void run_command(char *cmd);

static void cmd_whoami(void)
{
    printk("%s\n", user_shell_level() == USER_ADMIN
                        ? "admin (mozesz zmieniac system)"
                        : "uzytkownik (zmiana systemu przez 'sudo')");
}

/* sudo <komenda> - one command as an admin, and back down at once.
   sudo alone - an admin until 'wyloguj'. */
static void cmd_sudo(char *arg)
{
    if (user_shell_level() == USER_ADMIN) {
        if (*arg)
            run_command(arg);
        else
            printk("juz jestes adminem\n");
        return;
    }

    char pass[CONFIG_VAL_MAX];
    printk("haslo: ");
    read_secret(pass, sizeof(pass));
    if (!user_password_ok(pass)) {
        printk("sudo: zle haslo\n");
        return;
    }

    user_shell_set(USER_ADMIN);
    if (!*arg) {
        printk("jestes adminem; 'wyloguj' konczy\n");
        return;
    }
    run_command(arg);
    user_shell_set(USER_PLAIN);     /* one command, not a session */
}

static void cmd_logout(void)
{
    if (user_shell_level() != USER_ADMIN) {
        printk("nie jestes adminem\n");
        return;
    }
    user_shell_set(USER_PLAIN);
    printk("znowu zwykly uzytkownik\n");
}

/* Changing the password: the old one first, unless the person asking
   is already an admin - they have shown it once already. */
static void cmd_password(void)
{
    char pass[CONFIG_VAL_MAX], again[CONFIG_VAL_MAX];

    if (user_shell_level() != USER_ADMIN) {
        printk("stare haslo: ");
        read_secret(pass, sizeof(pass));
        if (!user_password_ok(pass)) {
            printk("haslo: zle haslo\n");
            return;
        }
    }
    printk("nowe haslo: ");
    read_secret(pass, sizeof(pass));
    if (!pass[0]) {
        printk("haslo: puste haslo to brak hasla - nie zmieniam\n");
        return;
    }
    printk("powtorz: ");
    read_secret(again, sizeof(again));
    if (strcmp(pass, again)) {
        printk("haslo: te dwa nie sa takie same - nie zmieniam\n");
        return;
    }
    if (user_set_password(pass) < 0) {
        printk("haslo: nie udalo sie zapisac (brak dysku?)\n");
        return;
    }
    printk("haslo zmienione\n");
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
    else if (!strcmp(cmd, "kim"))
        cmd_whoami();
    else if (!strcmp(cmd, "sudo"))
        cmd_sudo(arg);
    else if (!strcmp(cmd, "wyloguj"))
        cmd_logout();
    else if (!strcmp(cmd, "haslo"))
        cmd_password();
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
    else if (!strcmp(cmd, "mv"))
        cmd_mv(arg);
    else if (!strcmp(cmd, "set"))
        cmd_set(arg);
    else if (!strcmp(cmd, "smp"))
        cmd_smp();
    else if (!strcmp(cmd, "smpmem"))
        smpmem_test();
    else if (!strcmp(cmd, "smpthreads"))
        smpthreads_test();
    else if (!strcmp(cmd, "mem"))
        cmd_mem();
    else if (!strcmp(cmd, "ekran"))
        cmd_ekran();
    else if (!strcmp(cmd, "sprzet"))
        cmd_sprzet();
    else if (!strcmp(cmd, "formatuj"))
        cmd_formatuj(arg);
    else if (!strcmp(cmd, "instaluj"))
        cmd_install(arg);
    else if (!strcmp(cmd, "fat"))
        fatcmd_mount(arg);
    else if (!strcmp(cmd, "fatls"))
        fatcmd_ls(arg);
    else if (!strcmp(cmd, "fatcat"))
        fatcmd_cat(arg);
    else if (!strcmp(cmd, "fatput"))
        fatcmd_put(arg);
    else if (!strcmp(cmd, "fatmkdir"))
        fatcmd_mkdir(arg);
    else if (!strcmp(cmd, "fatrm"))
        fatcmd_rm(arg);
    else if (!strcmp(cmd, "fatsum"))
        fatcmd_sum(arg);
    else if (!strcmp(cmd, "fatcopy"))
        fatcmd_copy(arg);
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
    /* The clipboard is the system's, not one window's - so the console
       reaches it with the same two keys as everything else. */
    if (c == KEY_COPY) {
        if (sh->line_len)
            clip_set(sh->line, (uint32_t)sh->line_len);
        return;
    }
    if (c == KEY_PASTE) {
        char got[CLIP_MAX + 1];
        uint32_t n = clip_get(got, sizeof(got));
        for (uint32_t i = 0; i < n && sh->line_len < LINE_MAX - 1; i++) {
            if (got[i] < 0x20 || got[i] > 0x7E)
                continue;               /* one line, so no newlines */
            sh->line[sh->line_len++] = got[i];
            printk("%c", got[i]);
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
