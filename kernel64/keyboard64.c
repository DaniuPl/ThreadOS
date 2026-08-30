/* The PS/2 keyboard in long mode
 *
 * Scancode translation is shared with the 32-bit kernel
 * (kernel/kbdmap.c) - what stays here is only what depends on the
 * system: taking the interrupt and deciding where the finished
 * character goes.
 *
 * There are four consoles and each has its own character queue. A key
 * lands in the one we are currently looking at, and is read by
 * whoever is typing on it - so a program started on the second console
 * waits for its own keys and does not take somebody else's.
 * Alt+F1..F4 switches the picture.
 *
 * The interrupt only puts the character into the queue: inside its
 * handler there is nothing to print and nothing to wait for.
 */

#include <stdint.h>
#include "keyboard64.h"
#include "task64.h"
#include "../kernel/include/kbdmap.h"
#include "../kernel/include/io.h"
#include "../kernel/include/isr.h"
#include "../kernel/include/pic.h"
#include "../kernel/include/vga.h"
#include "signal64.h"

#define KBD_DATA_PORT 0x60
#define KBD_BUF_SIZE  128

/* One queue per console. Who gets Ctrl+C is a matter of the console
   too: the shell sets that for the time it waits for its child -
   outside that time there is nobody to interrupt. */
struct queue {
    char buf[KBD_BUF_SIZE];
    uint32_t head, tail;
    int intr_target;
};
static struct queue queues[CONSOLE_COUNT];
static int queues_ready;

static struct queue *console(int n)
{
    if (!queues_ready) {              /* -1 instead of zero for the Ctrl+C target */
        for (int i = 0; i < CONSOLE_COUNT; i++)
            queues[i].intr_target = -1;
        queues_ready = 1;
    }
    if (n < 0 || n >= CONSOLE_COUNT)
        n = 0;
    return &queues[n];
}

/* The desktop runs in graphics mode, where a text console makes no
   sense - so it intercepts all keys before they reach the queue the
   shell reads from. */
static void (*override)(char c);

void keyboard64_set_override(void (*handler)(char c))
{
    override = handler;
}

/* The Ctrl+C target is set by the shell of the console it types on
   itself */
void keyboard64_set_intr_target(int id)
{
    console(task64_vc())->intr_target = id;
}

/* Delivering a finished character. Called from the keyboard interrupt
   and from the USB keyboard thread - the character's path is the same
   in both cases. */
static void dorecz(char c)
{
    if (override) {                      /* the desktop takes everything */
        override(c);
        return;
    }

    /* The key goes to the console we are looking at - that is the one
       under the typist's fingers */
    struct queue *k = console(console_active());

    /* Ctrl+C is not a character to be read but a request to interrupt
       the program holding the console */
    if (c == KEY_INTR) {
        /* Not to the shell that is waiting, but to whatever is really
           running at the end of the chain */
        if (k->intr_target >= 0)
            task64_signal(task64_foreground(k->intr_target), SIGINT);
        return;
    }

    if (k->head - k->tail < KBD_BUF_SIZE)
        k->buf[k->head++ & (KBD_BUF_SIZE - 1)] = c;
}

/* Switching the picture to another console. The desktop holds the
   whole screen, so with it there is nothing to switch. */
static void switch_to(int vc)
{
    if (override)
        return;
    console_switch(vc);
}

static void keyboard_irq(struct regs *r)
{
    (void)r;

    char c = 0;
    int fkey = -1;
    int co = kbdmap_feed(inb(KBD_DATA_PORT), &c, &fkey);

    if (co == KBD_FKEY) {
        if (fkey >= 0 && fkey < CONSOLE_COUNT)
            switch_to(fkey);
        return;
    }
    if (co != KBD_CHAR)
        return;

    dorecz(c);
}

/* A character from a source other than PS/2 (a USB keyboard). Called
   from a thread, so we close the critical section ourselves. */
void keyboard64_inject(char c)
{
    irqflags_t flags = irq_save();
    dorecz(c);
    irq_restore(flags);
}

void keyboard64_inject_switch(int vc)
{
    irqflags_t flags = irq_save();
    switch_to(vc);
    irq_restore(flags);
}

/* Waits for a key. We poll with sleeping instead of blocking the task:
   with a single reader the difference is invisible and there is half
   as much code - waking from an interrupt will come along with the
   second reader. */
char keyboard64_getchar(void)
{
    for (;;) {
        irqflags_t flags = irq_save();
        struct queue *k = console(task64_vc());
        if (k->tail != k->head) {
            char c = k->buf[k->tail++ & (KBD_BUF_SIZE - 1)];
            irq_restore(flags);
            return c;
        }
        irq_restore(flags);
        task64_sleep(10);
    }
}

/* A read for a program in ring 3.
 *
 * It blocks until the first character, then hands over everything that
 * managed to accumulate - like every sensible input: we wait for
 * anything at all, not for a full buffer.
 *
 * A signal interrupts the wait and returns an error. Without that
 * Ctrl+C would have no way of reaching a program that is waiting for a
 * key - and that is exactly the program that needs Ctrl+C most
 * often. */
int keyboard64_read(char *out, uint32_t len)
{
    if (!len)
        return 0;

    for (;;) {
        irqflags_t flags = irq_save();
        /* A program reads from ITS OWN console, not from the one we happen
           to be looking at - otherwise switching the screen would
           redirect the keyboard in the middle of somebody's read */
        struct queue *k = console(task64_vc());
        uint32_t got = 0;
        while (k->tail != k->head && got < len)
            out[got++] = k->buf[k->tail++ & (KBD_BUF_SIZE - 1)];
        irq_restore(flags);

        if (got)
            return (int)got;
        if (task64_signal_pending())
            return -1;
        task64_sleep(10);
    }
}

int keyboard64_pending(void)
{
    struct queue *k = console(task64_vc());
    return k->tail != k->head;
}

void keyboard64_init(void)
{
    irq_install_handler(1, keyboard_irq);
    pic_unmask(1);
}
