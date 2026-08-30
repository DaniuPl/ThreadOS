#ifndef PROTONOS_VGA_H
#define PROTONOS_VGA_H

#include <stdint.h>
#include <stddef.h>

/* VGA text mode colours */
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

/* Virtual consoles: each has its own 80x25 screen, its own cursor and
   its own keyboard owner. One is visible; they are switched with
   Alt+F1..F4. Which one receives output is decided by the console of
   the writing task (task_vc). */
#define CONSOLE_COUNT 4

void console_switch(int n);
int  console_active(void);

/* Draw the console on the framebuffer instead of in the VGA character
   buffer - under UEFI there is no text mode at all */
void console_use_framebuffer(void);

void vga_init(void);
void vga_clear(void);
void vga_set_color(enum vga_color fg, enum vga_color bg);
void vga_putchar(char c);
void vga_write(const char *s);

/* Writes at a specific place on the screen without moving the cursor
   (e.g. the uptime counter in the corner) */
void vga_write_at(size_t x, size_t y, const char *s);

#endif
