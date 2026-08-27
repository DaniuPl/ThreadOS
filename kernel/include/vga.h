#ifndef PROTONOS_VGA_H
#define PROTONOS_VGA_H

#include <stdint.h>
#include <stddef.h>

/* Kolory trybu tekstowego VGA */
enum vga_color {
    VGA_BLACK = 0,
    VGA_BLUE = 1,
    VGA_GREEN = 2,
    VGA_CYAN = 3,
    VGA_RED = 4,
    VGA_MAGENTA = 5,
    VGA_BROWN = 6,
    VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8,
    VGA_LIGHT_BLUE = 9,
    VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_YELLOW = 14,
    VGA_WHITE = 15,
};

/* Konsole wirtualne: kazda ma wlasny ekran 80x25, wlasny kursor
   i wlasnego wlasciciela klawiatury. Widoczna jest jedna, przelacza
   sie je przez Alt+F1..F4. Ktora dostaje wypis, decyduje konsola
   piszacego zadania (task_vc). */
#define CONSOLE_COUNT 4

void console_switch(int n);
int  console_active(void);

void vga_init(void);
void vga_clear(void);
void vga_set_color(enum vga_color fg, enum vga_color bg);
void vga_putchar(char c);
void vga_write(const char *s);

/* Pisze w konkretnym miejscu ekranu, nie ruszając kursora
   (np. licznik uptime w rogu) */
void vga_write_at(size_t x, size_t y, const char *s);

#endif
