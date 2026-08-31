/* Translating PS/2 scancodes into characters (set 1)
 *
 * Split out of the keyboard driver, because it is the one part of it
 * that depends on nothing: neither on virtual consoles, nor on tasks,
 * nor on the processor mode. Thanks to that the same state machine
 * serves the keyboard in both kernels - and the decision where a
 * finished character goes stays with the driver.
 *
 * State has to live here, because one key press spreads over several
 * bytes: the 0xE0 prefix for extended keys, separate press and release
 * codes for the modifiers.
 */

#include "include/kbdmap.h"

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_CTRL   0x1D
#define SC_ALT    0x38
#define SC_TAB    0x0F
#define SC_F1     0x3B          /* F1..F12 lie consecutively from 0x3B */
#define SC_RELEASED 0x80
#define SC_EXTENDED 0xE0

/* The scancode -> ASCII map for the US layout (index = scancode) */
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

/* An extended scancode (after 0xE0) -> our control code */
static char extended_key(uint8_t scancode)
{
    switch (scancode) {
    case 0x48: return KEY_UP;
    case 0x50: return KEY_DOWN;
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x49: return KEY_PGUP;
    case 0x51: return KEY_PGDN;
    case 0x47: return KEY_HOME;
    case 0x4F: return KEY_END;
    default:   return 0;
    }
}

static int shift_pressed;
static int ctrl_pressed;
static int alt_pressed;
static int extended_next;               /* the previous byte was 0xE0 */

int kbdmap_feed(uint8_t scancode, char *out_char, int *fkey)
{
    if (scancode == SC_EXTENDED) {      /* the extended key prefix */
        extended_next = 1;
        return KBD_NOTHING;
    }
    if (extended_next) {
        extended_next = 0;
        if (!(scancode & SC_RELEASED)) {
            char c = extended_key(scancode);
            if (c) {
                *out_char = c;
                return KBD_CHAR;
            }
        }
        return KBD_NOTHING;
    }

    if (scancode == SC_ALT) {
        alt_pressed = 1;
        return KBD_NOTHING;
    }
    if (scancode == (SC_ALT | SC_RELEASED)) {
        alt_pressed = 0;
        return KBD_NOTHING;
    }
    /* Alt+Tab. A character it is not, and it never reaches a program:
       the desktop switches windows on it. Without a desktop nobody
       reads the code and nothing happens - which is what should happen
       on a text console. */
    if (alt_pressed && scancode == SC_TAB) {
        *out_char = KEY_NEXTWIN;
        return KBD_CHAR;
    }
    /* Alt plus a function key. What that means is the caller's
       decision - we only report the key number, counted from zero. */
    if (alt_pressed && scancode >= SC_F1 && scancode < SC_F1 + 12) {
        *fkey = scancode - SC_F1;
        return KBD_FKEY;
    }
    if (scancode == SC_CTRL) {
        ctrl_pressed = 1;
        return KBD_NOTHING;
    }
    if (scancode == (SC_CTRL | SC_RELEASED)) {
        ctrl_pressed = 0;
        return KBD_NOTHING;
    }
    if (scancode == SC_LSHIFT || scancode == SC_RSHIFT) {
        shift_pressed = 1;
        return KBD_NOTHING;
    }
    if (scancode == (SC_LSHIFT | SC_RELEASED) ||
        scancode == (SC_RSHIFT | SC_RELEASED)) {
        shift_pressed = 0;
        return KBD_NOTHING;
    }
    if (scancode & SC_RELEASED)         /* a key release - ignore */
        return KBD_NOTHING;

    char c = shift_pressed ? keymap_shift[scancode] : keymap[scancode];
    if (ctrl_pressed && c == 'c')
        c = KEY_INTR;
    /* Ctrl+D: not a character but 'that is all I am going to type'.
       Whoever is reading turns it into the end of the input. */
    if (ctrl_pressed && c == 'd')
        c = KEY_EOF;
    if (!c)
        return KBD_NOTHING;

    *out_char = c;
    return KBD_CHAR;
}
