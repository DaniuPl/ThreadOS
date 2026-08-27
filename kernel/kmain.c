/* ProtonOS - główny punkt wejścia jądra w C */

#include <stdint.h>
#include "include/vga.h"
#include "include/serial.h"
#include "include/printk.h"
#include "include/gdt.h"
#include "include/idt.h"
#include "include/pic.h"
#include "include/pit.h"
#include "include/keyboard.h"
#include "include/mouse.h"
#include "include/multiboot.h"
#include "include/pmm.h"
#include "include/paging.h"
#include "include/heap.h"
#include "include/shell.h"
#include "include/string.h"
#include "include/task.h"
#include "include/initrd.h"
#include "include/fs.h"
#include "include/blk.h"
#include "include/swap.h"
#include "include/desktop.h"
#include "include/net.h"
#include "include/rtl8139.h"
#include "include/dhcp.h"
#include "include/tcp.h"

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

/* Czy linia polecen jadra zawiera dane slowo?
   QEMU przekazuje ja opcja -append, GRUB - wpisem w grub.cfg. */
static int cmdline_has(const struct multiboot_info *mbi, const char *word)
{
    if (!(mbi->flags & MULTIBOOT_FLAG_CMDLINE) || !mbi->cmdline)
        return 0;

    const char *cmd = (const char *)mbi->cmdline;
    size_t wlen = strlen(word);

    for (int i = 0; cmd[i]; i++) {
        if (i && cmd[i - 1] != ' ')
            continue;                    /* dopasowuj tylko cale slowa */
        size_t k = 0;
        while (k < wlen && cmd[i + k] == word[k])
            k++;
        if (k == wlen && (cmd[i + k] == '\0' || cmd[i + k] == ' '))
            return 1;
    }
    return 0;
}

/* Szybki test sterty: alokacja, zapis, zwolnienie, ponowne uzycie */
static int kmalloc_selftest(void)
{
    char *a = kmalloc(64);
    char *b = kmalloc(128);
    if (!a || !b)
        return 0;
    memset(a, 0xAA, 64);
    memset(b, 0x55, 128);
    if ((uint8_t)a[63] != 0xAA || (uint8_t)b[0] != 0x55)
        return 0;
    kfree(a);
    char *c = kmalloc(32);   /* powinno trafic w zwolniona dziure */
    int reused = (c == a);
    kfree(b);
    kfree(c);
    return reused;
}

/* Wątki demonstracyjne: A i B liczą w swoim tempie, C wita się, żyje
   2 sekundy i kończy (test task_exit + sprzątania).
   Rysują w wierszu 0 (jak uptime) - kursor konsoli nigdy tam nie
   stoi, więc nie nadpisują promptu. */

static void draw_labeled_counter(int x, const char *label, uint32_t n)
{
    char buf[32];
    int i = 0;
    while (label[i]) {
        buf[i] = label[i];
        i++;
    }
    char digits[11];
    int d = 0;
    do {
        digits[d++] = '0' + n % 10;
        n /= 10;
    } while (n);
    while (d)
        buf[i++] = digits[--d];
    buf[i] = '\0';
    vga_write_at(x, 0, buf);
}

static void demo_counter(void *arg)
{
    int slot = (int)arg;
    uint32_t n = 0;
    for (;;) {
        draw_labeled_counter(slot ? 33 : 18,
                             slot ? "watek-B: " : "watek-A: ", n++);
        task_sleep(slot ? 330 : 200);
    }
}

static void demo_mortal(void *arg)
{
    (void)arg;
    vga_write_at(48, 0, "watek-C: czesc!     ");
    task_sleep(2000);
    /* Napis odrysowujemy jeszcze chwile. Wiersz 0 znika przy kazdym
       przewinieciu ekranu, a komunikaty startowe sypia sie jeszcze
       dobra sekunde po tym, jak watek konczy robote - inaczej dowod
       na to, ze zdazyl sie wykonac, wyjezdzalby razem z nimi. */
    for (int i = 0; i < 12; i++) {
        vga_write_at(48, 0, "watek-C: zakonczony ");
        task_sleep(500);
    }
}

static void ok(const char *what)
{
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    printk("[ OK ] ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    printk("%s\n", what);
}

void kmain(uint32_t magic, uint32_t multiboot_info_addr)
{
    const struct multiboot_info *mbi =
        (const struct multiboot_info *)multiboot_info_addr;

    vga_init();
    serial_init();

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    printk("ProtonOS\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    printk("Jadro uruchomione w trybie chronionym 32-bit.\n\n");

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        printk("BLAD: nieprawidlowy magic Multiboot (%x)!\n", magic);
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    ok("Multiboot OK.");

    gdt_init();
    ok("GDT zaladowana (plaski model pamieci, segmenty ring 0/3)");

    idt_init();
    ok("IDT zaladowana (wyjatki 0-31, IRQ 32-47)");

    pic_init();
    ok("PIC przemapowany (IRQ -> wektory 32-47)");

    pit_init(100);
    ok("Timer PIT: 100 Hz");

    keyboard_init();
    ok("Klawiatura PS/2 gotowa");

    mouse_init();
    ok("Mysz PS/2 gotowa (IRQ12)");

    __asm__ volatile("sti");
    ok("Przerwania wlaczone (sti)");

    /* Test IDT: int3 powinno wydrukowac komunikat i wrocic */
    __asm__ volatile("int $3");

    pmm_init(mbi);
    printk("[ OK ] Pamiec fizyczna: %u KiB RAM, %u wolnych ramek 4 KiB\n",
           pmm_ram_kib(), pmm_total_frames() - pmm_used_frames());

    paging_init();
    printk("[ OK ] Stronicowanie wlaczone (identycznosciowe %u MiB)\n",
           paging_identity_limit() / (1024 * 1024));

    heap_init();
    if (kmalloc_selftest())
        ok("Sterta jadra: kmalloc/kfree dzialaja (1 MiB)");
    else
        printk("[FAIL] kmalloc selftest!\n");

    initrd_init(mbi);
    if (initrd_file_count())
        printk("[ OK ] initrd: %u plikow (komendy 'ls' i 'run')\n",
               initrd_file_count());

    if (blk_init())
        printk("[ OK ] Kontroler dysku: %s\n", blk_name());
    if (fs_init())
        ok("Dysk + PFS gotowy (komendy 'fls', 'cat', 'write')");

    int has_net = net_init();
    if (has_net) {
        const uint8_t *m = rtl8139_mac();
        printk("[ OK ] Siec: RTL8139 %x:%x:%x:%x:%x:%x "
               "(komendy 'ifconfig', 'ping', 'dhcp')\n",
               m[0], m[1], m[2], m[3], m[4], m[5]);
    }

    scheduler_init();
    task_create("demo-A", demo_counter, (void *)0);
    task_create("demo-B", demo_counter, (void *)1);
    task_create("demo-C", demo_mortal, (void *)0);
    ok("Scheduler: watki jadra, wywlaszczanie co 10 ms");

    /* Wymiana stron dopiero tutaj: odzyskiwanie pamieci obchodzi liste
       zadan, wiec scheduler musi juz istniec */
    swap_init();

    /* Adres IP bierzemy z sieci - klient DHCP czeka na odpowiedzi
       serwera, wiec musi chodzic w watku (po scheduler_init) */
    if (has_net) {
        dhcp_start();
        tcp_init();        /* watek retransmisji TCP */
    }

    /* Docelowo system startuje prosto na pulpit; 'nodesktop' w linii
       polecen jadra zostawia konsole tekstowa (uzywa tego test dymny
       i komendy, ktore wypisuja duzo tekstu). Komunikat idzie przed
       shell_init, zeby ostatnia rzecza na ekranie byl prompt. */
    int text_mode = cmdline_has(mbi, "nodesktop") || cmdline_has(mbi, "text");
    if (text_mode)
        printk("Tryb tekstowy (linia polecen jadra) - pulpit: 'desktop'\n");
    else
        printk("Start pulpitu graficznego (wylaczysz opcja 'nodesktop')\n");

    shell_init();

    if (!text_mode)
        desktop_start();

    /* Watek main jest watkiem bezczynnosci - hlt do najblizszego IRQ */
    for (;;)
        __asm__ volatile("hlt");
}
