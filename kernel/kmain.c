/* ProtonOS - główny punkt wejścia jądra w C */

#include <stdint.h>
#include "include/vga.h"

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

void kmain(uint32_t magic, uint32_t multiboot_info_addr)
{
    (void)multiboot_info_addr; /* mapa pamięci itp. - do wykorzystania później */

    vga_init();

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_write("ProtonOS\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_write("Jadro uruchomione w trybie chronionym 32-bit.\n\n");

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_write("BLAD: nieprawidlowy magic Multiboot!\n");
    } else {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_write("Multiboot OK.\n");
    }

    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_write("\nNastepne kroki: GDT, IDT, przerwania, klawiatura...\n");

    /* Jądro nie ma dokąd wracać - czekamy w nieskończoność */
    for (;;)
        __asm__ volatile("hlt");
}
