#ifndef THREADOS_KBDMAP_H
#define THREADOS_KBDMAP_H

#include <stdint.h>
#include "keyboard.h"

/* Translating PS/2 scancodes (set 1) into characters
 *
 * A pure state machine: it takes byte after byte and says what came
 * out. It knows nothing about consoles, tasks or the desktop - and
 * that is why both kernels, 32- and 64-bit, use it. The decision WHERE
 * a character goes belongs to the caller.
 *
 * The state (Shift, Ctrl, Alt held down, the extended-key prefix) sits
 * inside, because it spreads over several consecutive bytes.
 */

#define KBD_NOTHING  0      /* a prefix, a modifier or a key release */
#define KBD_CHAR     1      /* an ASCII character or a KEY_* control code */
#define KBD_FKEY     2      /* Alt + a function key (the number in *fkey) */

/* Feeds one byte from the driver. Returns one of the constants above. */
int kbdmap_feed(uint8_t scancode, char *out_char, int *fkey);

#endif
