/* VGA text mode and virtual consoles
 *
 * The hardware has one screen (the buffer at 0xB8000), but the system
 * keeps several: every console has its own 80x25 characters, its own
 * cursor and its own colour. One is visible - its writes go both into
 * its own buffer and into the card's memory; the others write only
 * into their own and wait their turn (Alt+F1..F4).
 *
 * Which console receives output is decided by the TASK doing the
 * writing - just as in Unix a terminal is a property of a process
 * rather than the other way round. Thanks to that a program started on
 * the second console keeps writing to it even while we are looking at
 * the first.
 */

#include "include/vga.h"
#include "include/string.h"
#include "include/task.h"
#include "include/fbcon.h"

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

/* When there is no VGA text mode (e.g. under UEFI), we draw the
   characters ourselves on the framebuffer. The console character
   buffer stays unchanged - all that differs is where a finished cell
   goes. */
static int on_framebuffer;
static int screen_taken;

static uint16_t vga_entry(char c, uint8_t color)
{
    return (uint16_t)c | (uint16_t)color << 8;
}

/* The current task's console. Before the scheduler starts (boot
   messages) task_vc() returns zero - the first console. */
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
    if (c != &consoles[active])
        return;
    if (on_framebuffer && !screen_taken)
        fbcon_cell((int)(idx % VGA_WIDTH), (int)(idx / VGA_WIDTH),
                           (char)(cell & 0xFF), (uint8_t)(cell >> 8));
    else if (!on_framebuffer)
        vga_buffer[idx] = cell;
}

/* Redraws the whole visible console - after a scroll and after
   switching the screen */
static void redraw(const struct console *c)
{
    if (on_framebuffer) {
        if (screen_taken)
            return;
        for (size_t i = 0; i < VGA_CELLS; i++)
            fbcon_cell((int)(i % VGA_WIDTH), (int)(i / VGA_WIDTH),
                               (char)(c->cells[i] & 0xFF),
                               (uint8_t)(c->cells[i] >> 8));
    } else {
        for (size_t i = 0; i < VGA_CELLS; i++)
            vga_buffer[i] = c->cells[i];
    }
}

/* Turns on drawing consoles on the framebuffer. Called at startup,
   before anything is printed. */
void console_use_framebuffer(void)
{
    if (fbcon_enable())
        on_framebuffer = 1;
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
    redraw(&consoles[0]);
}

int console_active(void)
{
    return active;
}

/* Switches the visible console - called from the keyboard interrupt */
void console_switch(int n)
{
    if (n < 0 || n >= CONSOLE_COUNT || n == active)
        return;
    active = n;
    redraw(&consoles[n]);
}

/* Scrolls a console one line up */
static void scroll(struct console *c)
{
    for (size_t i = VGA_WIDTH; i < VGA_CELLS; i++)
        c->cells[i - VGA_WIDTH] = c->cells[i];
    for (size_t x = 0; x < VGA_WIDTH; x++)
        c->cells[VGA_CELLS - VGA_WIDTH + x] = vga_entry(' ', c->color);
    c->row = VGA_HEIGHT - 1;

    if (c == &consoles[active])
        redraw(c);
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

/* Whether anything is left of the text screen.
 *
 * The corner of the text screen was where the boot-time diagnostics
 * went: the mouse position, the uptime, a demonstration thread saying
 * hello. Under a framebuffer there is no text screen - the characters
 * are DRAWN, straight onto the buffer the monitor shows - so once the
 * compositor owns that buffer those writes are somebody scribbling on
 * the desktop. From a mouse interrupt, at packet rate.
 *
 * So the compositor says it has taken the screen, and console output
 * quietly stops drawing there. The serial log is untouched: that is
 * where a diagnostic belongs anyway. */

void vga_screen_taken(int yes)
{
    screen_taken = yes;
}

void vga_write_at(size_t x, size_t y, const char *s)
{
    if (screen_taken)
        return;
    struct console *con = mine();
    while (*s && x < VGA_WIDTH && y < VGA_HEIGHT) {
        put(con, y * VGA_WIDTH + x, vga_entry(*s, con->color));
        x++;
        s++;
    }
}
