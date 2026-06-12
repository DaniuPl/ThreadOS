/* Sterownik trybu tekstowego VGA (80x25, bufor pod 0xB8000) */

#include "include/vga.h"
#include "include/string.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

static volatile uint16_t *const vga_buffer = (uint16_t *)0xB8000;
static size_t row;
static size_t col;
static uint8_t color;

static uint16_t vga_entry(char c, uint8_t color)
{
    return (uint16_t)c | (uint16_t)color << 8;
}

void vga_set_color(enum vga_color fg, enum vga_color bg)
{
    color = (uint8_t)fg | (uint8_t)bg << 4;
}

void vga_clear(void)
{
    row = 0;
    col = 0;
    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            vga_buffer[y * VGA_WIDTH + x] = vga_entry(' ', color);
}

void vga_init(void)
{
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_clear();
}

/* Przewija ekran o jedną linię w górę */
static void vga_scroll(void)
{
    for (size_t y = 1; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            vga_buffer[(y - 1) * VGA_WIDTH + x] = vga_buffer[y * VGA_WIDTH + x];

    for (size_t x = 0; x < VGA_WIDTH; x++)
        vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', color);

    row = VGA_HEIGHT - 1;
}

void vga_putchar(char c)
{
    if (c == '\n') {
        col = 0;
        if (++row == VGA_HEIGHT)
            vga_scroll();
        return;
    }
    if (c == '\b') {
        if (col > 0) {
            col--;
            vga_buffer[row * VGA_WIDTH + col] = vga_entry(' ', color);
        }
        return;
    }

    vga_buffer[row * VGA_WIDTH + col] = vga_entry(c, color);
    if (++col == VGA_WIDTH) {
        col = 0;
        if (++row == VGA_HEIGHT)
            vga_scroll();
    }
}

void vga_write(const char *s)
{
    while (*s)
        vga_putchar(*s++);
}

void vga_write_at(size_t x, size_t y, const char *s)
{
    while (*s && x < VGA_WIDTH && y < VGA_HEIGHT)
        vga_buffer[y * VGA_WIDTH + x++] = vga_entry(*s++, color);
}
