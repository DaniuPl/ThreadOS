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

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

static void ok(const char *what)
{
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    printk("[ OK ] ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    printk("%s\n", what);
}

void kmain(uint32_t magic, uint32_t multiboot_info_addr)
{
    (void)multiboot_info_addr; /* mapa pamięci itp. - do wykorzystania później */

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

    __asm__ volatile("sti");
    ok("Przerwania wlaczone (sti)");

    /* Test IDT: int3 powinno wydrukowac komunikat i wrocic */
    __asm__ volatile("int $3");

    printk("\nSystem gotowy. Mozesz pisac na klawiaturze:\n> ");

    /* Czekaj na przerwania - hlt budzi CPU przy kazdym IRQ */
    for (;;)
        __asm__ volatile("hlt");
}
