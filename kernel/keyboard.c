/* Sterownik klawiatury PS/2 (scancode set 1)
 *
 * Na razie najprostsza możliwa wersja: każde wciśnięcie klawisza
 * jest tłumaczone na ASCII i echo'wane na ekran.
 */

#include <stdint.h>
#include "include/keyboard.h"
#include "include/io.h"
#include "include/isr.h"
#include "include/pic.h"

#define KBD_DATA_PORT 0x60

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_RELEASED 0x80

/* Mapa scancode -> ASCII dla układu US (indeks = scancode) */
static const char keymap[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0 /* ctrl */, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0 /* lshift */, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0 /* rshift */, '*', 0 /* alt */, ' ',
};

static const char keymap_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0, '*', 0, ' ',
};

static int shift_pressed;
static keyboard_char_handler_t on_char;

void keyboard_set_handler(keyboard_char_handler_t handler)
{
    on_char = handler;
}

static void keyboard_irq(struct regs *r)
{
    (void)r;
    uint8_t scancode = inb(KBD_DATA_PORT);

    if (scancode == SC_LSHIFT || scancode == SC_RSHIFT) {
        shift_pressed = 1;
        return;
    }
    if (scancode == (SC_LSHIFT | SC_RELEASED) ||
        scancode == (SC_RSHIFT | SC_RELEASED)) {
        shift_pressed = 0;
        return;
    }
    if (scancode & SC_RELEASED)  /* puszczenie klawisza - ignoruj */
        return;

    char c = shift_pressed ? keymap_shift[scancode] : keymap[scancode];
    if (c && on_char)
        on_char(c);
}

void keyboard_init(void)
{
    irq_install_handler(1, keyboard_irq);
    pic_unmask(1);
}
