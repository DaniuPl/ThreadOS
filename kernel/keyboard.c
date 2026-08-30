/* PS/2 keyboard driver (scancode set 1)
 *
 * Every key press is translated into ASCII and passed on. Keys with no
 * ASCII equivalent - arrows, PgUp/PgDn, Home/End - arrive as a
 * scancode preceded by the byte 0xE0 and are turned into control codes
 * of our own (KEY_* in keyboard.h).
 */

#include <stdint.h>
#include "include/keyboard.h"
#include "include/kbdmap.h"
#include "include/io.h"
#include "include/isr.h"
#include "include/pic.h"
#include "include/task.h"
#include "include/signal.h"
#include "include/vga.h"

#define KBD_DATA_PORT 0x60

/* Every virtual console has its own character queue and its own owner.
   When some process reads the keyboard from it (sys_read), characters
   land in the queue instead of at that console's shell; once the owner
   dies the console goes back to the shell. */
#define KBD_BUF_SIZE 128

struct kbd_console {
    char buf[KBD_BUF_SIZE];
    uint32_t head, tail;
    int owner_id;                       /* the reading process, -1 = the shell */
    struct task *waiter;
    keyboard_char_handler_t on_char;    /* this console's shell */
};

static struct kbd_console cons[CONSOLE_COUNT];
static keyboard_char_handler_t override;   /* the desktop takes all keys */
static int cons_ready;

static struct kbd_console *console(int n)
{
    if (!cons_ready) {                  /* -1 rather than zero for the owner */
        for (int i = 0; i < CONSOLE_COUNT; i++)
            cons[i].owner_id = -1;
        cons_ready = 1;
    }
    if (n < 0 || n >= CONSOLE_COUNT)
        n = 0;
    return &cons[n];
}

void keyboard_set_handler(int vc, keyboard_char_handler_t handler)
{
    console(vc)->on_char = handler;
}

/* The desktop runs in graphics mode, where text consoles make no
   sense - so it grabs all keys before they go anywhere */
void keyboard_set_override(keyboard_char_handler_t handler)
{
    override = handler;
}

keyboard_char_handler_t keyboard_console_handler(void)
{
    return console(console_active())->on_char;
}

/* Delivering a character - IRQ context (IF=0). The character goes to
   the console currently visible; that console decides whether it
   belongs to a process or to the shell. */
static void deliver(char c)
{
    if (override) {                     /* the desktop */
        override(c);
        return;
    }

    struct kbd_console *k = console(console_active());
    if (k->owner_id >= 0 && !task_alive(k->owner_id))
        k->owner_id = -1;

    /* Ctrl+C is not a character to be read but a signal for the
       process holding the console. When nobody holds the console there
       is nobody to interrupt - the kernel shell ignores the
       character. */
    if (c == KEY_INTR && k->owner_id >= 0) {
        task_signal_send(k->owner_id, SIGINT);
        return;
    }

    if (k->owner_id >= 0) {
        if (k->head - k->tail < KBD_BUF_SIZE)
            k->buf[k->head++ & (KBD_BUF_SIZE - 1)] = c;
        if (k->waiter) {
            task_unblock(k->waiter);
            k->waiter = (void *)0;
        }
    } else if (k->on_char) {
        k->on_char(c);
    }
}

/* Injecting a character from a source other than PS/2 (a USB
   keyboard). Called from a thread, so we close the critical section
   ourselves - the character's road onward is exactly the same as with
   a keyboard interrupt. */
void keyboard_inject(char c)
{
    uint32_t flags = irq_save();
    deliver(c);
    irq_restore(flags);
}

/* Switching consoles from another source (Alt+F1..F4 on a USB keyboard) */
void keyboard_inject_switch(int vc)
{
    if (override)                       /* the desktop holds the whole screen */
        return;
    uint32_t flags = irq_save();
    console_switch(vc);
    irq_restore(flags);
}

/* A blocking read for the read syscall - called by a user process */
int keyboard_read(char *buf, uint32_t len)
{
    if (!len)
        return 0;

    uint32_t flags = irq_save();
    /* A process reads from ITS OWN console, not from the one we
       happen to be looking at - otherwise switching the screen would
       redirect the keyboard in the middle of somebody's read */
    struct kbd_console *k = console(task_vc());
    k->owner_id = task_current_id();

    uint32_t got = 0;
    int interrupted = 0;
    while (!got) {
        while (k->tail != k->head && got < len)
            buf[got++] = k->buf[k->tail++ & (KBD_BUF_SIZE - 1)];
        if (got)
            break;
        /* Waiting for a key is interrupted by a signal (e.g. Ctrl+C): we
           return with an error, so that the process gets a chance to
           enter its handler */
        if (task_signal_pending()) {
            interrupted = 1;
            break;
        }
        k->waiter = task_current();
        task_block();
    }

    if (k->waiter == task_current())    /* a signal woke us, not a key */
        k->waiter = (void *)0;
    irq_restore(flags);
    return interrupted ? -1 : (int)got;
}

/* The IRQ1 handler: we leave the translation to kbdmap and keep the
   decision of where the character goes for ourselves */
static void keyboard_irq(struct regs *r)
{
    (void)r;

    char c = 0;
    int fkey = -1;
    switch (kbdmap_feed(inb(KBD_DATA_PORT), &c, &fkey)) {
    case KBD_FKEY:
        /* Alt+F1..F4 switches consoles. In graphics mode there is
           nothing to switch - the desktop keeps the whole screen to
           itself. */
        if (!override && fkey >= 0 && fkey < CONSOLE_COUNT)
            console_switch(fkey);
        break;
    case KBD_CHAR:
        deliver(c);
        break;
    default:
        break;
    }
}

void keyboard_init(void)
{
    irq_install_handler(1, keyboard_irq);
    pic_unmask(1);
}
