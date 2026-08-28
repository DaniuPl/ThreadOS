/* Tlumaczenie scancode'ow PS/2 na znaki (zestaw 1)
 *
 * Wydzielone ze sterownika klawiatury, bo to jedyna jego czesc, ktora
 * nie zalezy od niczego: ani od konsol wirtualnych, ani od zadan, ani
 * od trybu procesora. Dzieki temu ten sam automat obsluguje klawiature
 * w obu jadrach - a decyzja, dokad trafi gotowy znak, zostaje po
 * stronie sterownika.
 *
 * Stan musi tu byc, bo jedno wcisniecie klawisza rozklada sie na kilka
 * bajtow: prefiks 0xE0 dla klawiszy rozszerzonych, osobne kody
 * wcisniecia i puszczenia modyfikatorow.
 */

#include "include/kbdmap.h"

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_CTRL   0x1D
#define SC_ALT    0x38
#define SC_F1     0x3B          /* F1..F12 leza kolejno od 0x3B */
#define SC_RELEASED 0x80
#define SC_EXTENDED 0xE0

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

/* Scancode rozszerzony (po 0xE0) -> nasz kod sterujacy */
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
static int extended_next;               /* poprzedni bajt to 0xE0 */

int kbdmap_feed(uint8_t scancode, char *out_char, int *fkey)
{
    if (scancode == SC_EXTENDED) {      /* prefiks klawisza rozszerzonego */
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
    /* Alt + klawisz funkcyjny. Co to znaczy, decyduje wolajacy - my
       podajemy tylko numer klawisza, liczony od zera. */
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
    if (scancode & SC_RELEASED)         /* puszczenie klawisza - ignoruj */
        return KBD_NOTHING;

    char c = shift_pressed ? keymap_shift[scancode] : keymap[scancode];
    if (ctrl_pressed && c == 'c')       /* pozostale kombinacje z Ctrl - jeszcze nie */
        c = KEY_INTR;
    if (!c)
        return KBD_NOTHING;

    *out_char = c;
    return KBD_CHAR;
}
