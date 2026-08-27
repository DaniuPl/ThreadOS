/* Tryb tekstowy VGA i konsole wirtualne
 *
 * Sprzet ma jeden ekran (bufor pod 0xB8000), ale system trzyma ich
 * kilka: kazda konsola ma wlasne 80x25 znakow, wlasny kursor i wlasny
 * kolor. Widoczna jest jedna - jej zapisy ida rownolegle do wlasnego
 * bufora i do pamieci karty; pozostale pisza tylko u siebie i czekaja
 * na swoja kolej (Alt+F1..F4).
 *
 * Ktora konsola dostaje wypis, decyduje ZADANIE, ktore pisze - tak jak
 * w uniksie terminal jest wlasnoscia procesu, a nie odwrotnie. Dzieki
 * temu program uruchomiony na drugiej konsoli pisze na niej dalej,
 * nawet gdy patrzymy na pierwsza.
 */

#include "include/vga.h"
#include "include/string.h"
#include "include/task.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_CELLS  (VGA_WIDTH * VGA_HEIGHT)

static volatile uint16_t *const vga_buffer = (uint16_t *)0xB8000;

struct console {
    uint16_t cells[VGA_CELLS];
    size_t row, col;
    uint8_t color;
};

static struct console consoles[CONSOLE_COUNT];
static int active;

static uint16_t vga_entry(char c, uint8_t color)
{
    return (uint16_t)c | (uint16_t)color << 8;
}

/* Konsola biezacego zadania. Przed startem schedulera (komunikaty
   rozruchowe) task_vc() zwraca zero - pierwsza konsola. */
static struct console *mine(void)
{
    int n = task_vc();
    if (n < 0 || n >= CONSOLE_COUNT)
        n = 0;
    return &consoles[n];
}

static void put(struct console *c, size_t idx, uint16_t cell)
{
    c->cells[idx] = cell;
    if (c == &consoles[active])
        vga_buffer[idx] = cell;
}

void vga_set_color(enum vga_color fg, enum vga_color bg)
{
    mine()->color = (uint8_t)fg | (uint8_t)bg << 4;
}

void vga_clear(void)
{
    struct console *c = mine();
    c->row = 0;
    c->col = 0;
    for (size_t i = 0; i < VGA_CELLS; i++)
        put(c, i, vga_entry(' ', c->color));
}

void vga_init(void)
{
    for (int i = 0; i < CONSOLE_COUNT; i++) {
        consoles[i].color = (uint8_t)VGA_LIGHT_GREY | (uint8_t)VGA_BLACK << 4;
        consoles[i].row = 0;
        consoles[i].col = 0;
        for (size_t k = 0; k < VGA_CELLS; k++)
            consoles[i].cells[k] = vga_entry(' ', consoles[i].color);
    }
    active = 0;
    for (size_t k = 0; k < VGA_CELLS; k++)
        vga_buffer[k] = consoles[0].cells[k];
}

int console_active(void)
{
    return active;
}

/* Przelacza widoczna konsole - wolane z przerwania klawiatury */
void console_switch(int n)
{
    if (n < 0 || n >= CONSOLE_COUNT || n == active)
        return;
    active = n;
    for (size_t k = 0; k < VGA_CELLS; k++)
        vga_buffer[k] = consoles[n].cells[k];
}

/* Przewija konsole o jedna linie w gore */
static void scroll(struct console *c)
{
    for (size_t i = VGA_WIDTH; i < VGA_CELLS; i++)
        c->cells[i - VGA_WIDTH] = c->cells[i];
    for (size_t x = 0; x < VGA_WIDTH; x++)
        c->cells[VGA_CELLS - VGA_WIDTH + x] = vga_entry(' ', c->color);
    c->row = VGA_HEIGHT - 1;

    if (c == &consoles[active])
        for (size_t k = 0; k < VGA_CELLS; k++)
            vga_buffer[k] = c->cells[k];
}

void vga_putchar(char c)
{
    struct console *con = mine();

    if (c == '\n') {
        con->col = 0;
        if (++con->row == VGA_HEIGHT)
            scroll(con);
        return;
    }
    if (c == '\b') {
        if (con->col > 0) {
            con->col--;
            put(con, con->row * VGA_WIDTH + con->col,
                vga_entry(' ', con->color));
        }
        return;
    }

    put(con, con->row * VGA_WIDTH + con->col, vga_entry(c, con->color));
    if (++con->col == VGA_WIDTH) {
        con->col = 0;
        if (++con->row == VGA_HEIGHT)
            scroll(con);
    }
}

void vga_write(const char *s)
{
    while (*s)
        vga_putchar(*s++);
}

void vga_write_at(size_t x, size_t y, const char *s)
{
    struct console *con = mine();
    while (*s && x < VGA_WIDTH && y < VGA_HEIGHT) {
        put(con, y * VGA_WIDTH + x, vga_entry(*s, con->color));
        x++;
        s++;
    }
}
