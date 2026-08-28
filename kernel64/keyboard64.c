/* Klawiatura PS/2 w trybie dlugim
 *
 * Tlumaczenie scancode'ow jest wspolne z jadrem 32-bitowym
 * (kernel/kbdmap.c) - tutaj zostaje tylko to, co zalezy od systemu:
 * odebranie przerwania i zdecydowanie, dokad trafi gotowy znak.
 *
 * Konsola jest na razie jedna, wiec decyzja jest krotka: znak idzie do
 * shella. Przerwanie tylko wklada go do kolejki i budzi czekajacego -
 * w obsludze przerwania nie ma czego drukowac ani na co czekac.
 */

#include <stdint.h>
#include "keyboard64.h"
#include "task64.h"
#include "../kernel/include/kbdmap.h"
#include "../kernel/include/io.h"
#include "../kernel/include/isr.h"
#include "../kernel/include/pic.h"

#define KBD_DATA_PORT 0x60
#define KBD_BUF_SIZE  128

static char buf[KBD_BUF_SIZE];
static uint32_t head, tail;

static void keyboard_irq(struct regs *r)
{
    (void)r;

    char c = 0;
    int fkey = -1;
    if (kbdmap_feed(inb(KBD_DATA_PORT), &c, &fkey) != KBD_CHAR)
        return;

    if (head - tail < KBD_BUF_SIZE)
        buf[head++ & (KBD_BUF_SIZE - 1)] = c;
}

/* Czeka na klawisz. Odpytujemy z uspieniem zamiast blokowac zadanie:
   przy jednym czytelniku roznica jest niewidoczna, a kodu o polowe
   mniej - budzenie z przerwania przyjdzie razem z drugim czytelnikiem. */
char keyboard64_getchar(void)
{
    for (;;) {
        irqflags_t flags = irq_save();
        if (tail != head) {
            char c = buf[tail++ & (KBD_BUF_SIZE - 1)];
            irq_restore(flags);
            return c;
        }
        irq_restore(flags);
        task64_sleep(10);
    }
}

int keyboard64_pending(void)
{
    return tail != head;
}

void keyboard64_init(void)
{
    irq_install_handler(1, keyboard_irq);
    pic_unmask(1);
}
