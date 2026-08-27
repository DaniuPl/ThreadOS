/* ProtonOS 64 - pierwszy krok portu na tryb dlugi
 *
 * Do tego miejsca procesor doszedl juz przez boot64/boot64.S: ma
 * wlaczony tryb dlugi, wlasna tablice deskryptorow i pierwsze cztery
 * gigabajty zamapowane identycznosciowo stronami po 2 MiB.
 *
 * Ten plik jest jeszcze szkieletem. Wazne jest to, czego tu NIE MA:
 * zadnej kopii kodu z jadra 32-bitowego. Wypisywanie, konsola na
 * buforze obrazu, odczyt znacznikow Multiboota 2 i obsluga lancuchow
 * to te same pliki, kompilowane drugi raz - roznice miedzy trybami
 * zamykaja sie w naglowku io.h i w kilku rzutowaniach.
 */

#include <stdint.h>
#include "../kernel/include/mb2.h"
#include "../kernel/include/fbcon.h"
#include "../kernel/include/vga.h"
#include "../kernel/include/serial.h"
#include "../kernel/include/printk.h"
#include "../kernel/include/pic.h"
#include "../kernel/include/pit.h"
#include "idt64.h"

/* Miejsca, w ktorych wspoldzielone pliki siegaja po scheduler.
   Zadan w tym jadrze jeszcze nie ma, wiec wypisy ida na pierwsza
   konsole, a przerwanie timera tylko zwieksza licznik i wraca.
   Te dwie zaslepki znikna, gdy port dojdzie do zadan. */
int task_vc(void)
{
    return 0;
}

void schedule(void)
{
}

/* Czy procesor naprawde jest w trybie dlugim? Pyta o to sam siebie:
   bit LMA w rejestrze EFER ustawia procesor, a nie my. */
static int long_mode_active(void)
{
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080));
    (void)high;
    return (low & (1u << 10)) != 0;
}

void kmain64(uint32_t magic, uint32_t info_addr)
{
    const struct multiboot_info *mbi = mb2_info(magic, info_addr);

    const struct mb2_framebuffer *lfb = mb2_framebuffer();
    if (lfb && fbcon_set_framebuffer(lfb->addr, lfb->pitch, (int)lfb->width,
                                     (int)lfb->height, (int)lfb->bpp) == 0)
        console_use_framebuffer();

    vga_init();
    serial_init();

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    printk("ProtonOS 64\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    printk("Jadro uruchomione w trybie dlugim (x86-64).\n\n");

    if (!mbi) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        printk("BLAD: nieznany magic od bootloadera (%x)!\n", magic);
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    printk("[ OK ] Multiboot OK (%s)\n", mb2_boot_kind());

    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    printk("[ OK ] ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    printk("Tryb dlugi: LMA=%u, wskaznik ma %u bajtow\n",
           (uint32_t)long_mode_active(), (uint32_t)sizeof(void *));

    /* Mapa pamieci ze znacznikow - ten sam kod, co w jadrze 32-bitowym */
    uint32_t ram_kib = 0;
    if (mbi->flags & MULTIBOOT_FLAG_MMAP) {
        uint32_t pos = mbi->mmap_addr;
        uint32_t end = mbi->mmap_addr + mbi->mmap_length;
        while (pos < end) {
            const struct multiboot_mmap_entry *e =
                (const struct multiboot_mmap_entry *)(uintptr_t)pos;
            if (e->type == MULTIBOOT_MEMORY_AVAILABLE)
                ram_kib += (uint32_t)(e->len / 1024);
            pos += e->size + 4;
        }
    }
    printk("[ OK ] Pamiec fizyczna: %u KiB RAM\n", ram_kib);

    if (mbi->flags & MULTIBOOT_FLAG_MODS)
        printk("[ OK ] Moduly od loadera: %u\n", mbi->mods_count);

    if (fbcon_have())
        printk("[ OK ] Bufor obrazu: %ux%u, %u bajtow na wiersz\n",
               (uint32_t)fbcon_width(), (uint32_t)fbcon_height(),
               (uint32_t)fbcon_pitch());

    if (mb2_rsdp())
        printk("[ OK ] Wskaznik ACPI od loadera: %x\n", mb2_rsdp());

    idt64_init();
    printk("[ OK ] IDT zaladowana (wyjatki 0-31, IRQ 32-47)\n");

    pic_init();
    printk("[ OK ] PIC przemapowany (IRQ -> wektory 32-47)\n");

    pit_init(100);
    printk("[ OK ] Timer PIT: 100 Hz\n");

    __asm__ volatile("sti");
    printk("[ OK ] Przerwania wlaczone (sti)\n");

    /* Wyjatek-pulapka: sprawdza cala droge - stub, tablice, ramke
       rejestrow i powrot przez iretq - nie zatrzymujac systemu */
    __asm__ volatile("int $3");

    /* Timer musi tykac sam z siebie: czekamy na przerwania i patrzymy,
       czy licznik rusza. To dowod, ze przerwania sprzetowe dochodza. */
    uint32_t start = pit_ticks();
    for (int i = 0; i < 200; i++)
        __asm__ volatile("hlt");
    printk("[ OK ] Timer tyka: %u -> %u tyknac\n", start, pit_ticks());

    printk("\nSzkielet 64-bitowy stoi. Reszta jadra dochodzi po kolei.\n");

    for (;;)
        __asm__ volatile("hlt");
}
