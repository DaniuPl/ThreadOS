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
/* Insert is the only extended key whose meaning depends on what is
   held down with it, so it is decided where the state is known. */
static char extended_key(uint8_t scancode);

static int shift_pressed;
static int ctrl_pressed;
static int alt_pressed;

static char extended_key(uint8_t scancode)
{
    if (scancode == 0x52) {              /* Insert */
        if (ctrl_pressed)
            return KEY_COPY;
        if (shift_pressed)
            return KEY_PASTE;
        return 0;                        /* plain Insert: nothing yet */
    }
    /* Ctrl+Alt and an arrow changes the desktop - the arrow points
       where you are going. With Shift the window that has the focus
       comes along, because a window left behind is a window looked
       for. */
    if (scancode == 0x4B || scancode == 0x4D) {
        if (ctrl_pressed && alt_pressed) {
            if (scancode == 0x4B)
                return shift_pressed ? KEY_DESK_TAKE_PREV : KEY_DESK_PREV;
            return shift_pressed ? KEY_DESK_TAKE_NEXT : KEY_DESK_NEXT;
        }
    }
    /* Print Screen arrives as two extended pairs (E0 2A E0 37); the
       0x2A half is the shift the keyboard sends on its own and means
       nothing here, so the 0x37 is the whole of it. */
    if (scancode == 0x37)
        return KEY_SHOT;
    /* The key with a window on it, left or right: on this desktop, as
       on others, it opens the menu */
    if (scancode == 0x5B || scancode == 0x5C)
        return KEY_MENU;
    if (scancode == 0x30)
        return KEY_VOLUP;
    if (scancode == 0x2E)
        return KEY_VOLDOWN;
    if (scancode == 0x20)
        return KEY_MUTE;

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

    /* Ctrl+S and Ctrl+O keep their own codes: a program that writes
       files has to be able to tell "save" from the letter s. */
    if (ctrl_pressed && c == 's')
        c = KEY_SAVE;
    if (ctrl_pressed && c == 'o')
        c = KEY_OPEN;
    if (ctrl_pressed && c == 'f')
        c = KEY_FIND;
    if (ctrl_pressed && c == 'z')
        c = KEY_UNDO;
    if (ctrl_pressed && c == ' ')
        c = KEY_SEARCH;
    if (ctrl_pressed && alt_pressed && c == 'l')
        c = KEY_LOCK;
    *out_char = c;
    return KBD_CHAR;
}
