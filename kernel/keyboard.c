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
#include "include/task.h"

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

/* Konsola dla procesow: gdy jakis proces czyta klawiature
   (sys_read), znaki ladaja w buforze kolowym zamiast w shellu
   jadra. Po smierci wlasciciela konsola wraca do jadra. */
#define KBD_BUF_SIZE 128
static char kbuf[KBD_BUF_SIZE];
static uint32_t khead, ktail;
static int console_owner_id = -1;
static struct task *kbd_waiter;

void keyboard_set_handler(keyboard_char_handler_t handler)
{
    on_char = handler;
}

keyboard_char_handler_t keyboard_get_handler(void)
{
    return on_char;
}

/* Dostarczenie znaku - kontekst IRQ (IF=0) */
static void deliver(char c)
{
    if (console_owner_id >= 0 && !task_alive(console_owner_id))
        console_owner_id = -1;

    if (console_owner_id >= 0) {
        if (khead - ktail < KBD_BUF_SIZE)
            kbuf[khead++ & (KBD_BUF_SIZE - 1)] = c;
        if (kbd_waiter) {
            task_unblock(kbd_waiter);
            kbd_waiter = (void *)0;
        }
    } else if (on_char) {
        on_char(c);
    }
}

/* Blokujacy odczyt dla syscalla read - wola proces uzytkownika */
int keyboard_read(char *buf, uint32_t len)
{
    if (!len)
        return 0;

    uint32_t flags = irq_save();
    console_owner_id = task_current_id();

    uint32_t got = 0;
    while (!got) {
        while (ktail != khead && got < len)
            buf[got++] = kbuf[ktail++ & (KBD_BUF_SIZE - 1)];
        if (!got) {
            kbd_waiter = task_current();
            task_block();
        }
    }

    irq_restore(flags);
    return (int)got;
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
    if (c)
        deliver(c);
}

void keyboard_init(void)
{
    irq_install_handler(1, keyboard_irq);
    pic_unmask(1);
}
