/* Sterownik klawiatury PS/2 (scancode set 1)
 *
 * Każde wciśnięcie klawisza jest tłumaczone na ASCII i przekazywane
 * dalej. Klawisze bez odpowiednika w ASCII - strzałki, PgUp/PgDn,
 * Home/End - przychodzą jako scancode poprzedzony bajtem 0xE0
 * i zamieniane są na własne kody sterujące (KEY_* w keyboard.h).
 */

#include <stdint.h>
#include "include/keyboard.h"
#include "include/io.h"
#include "include/isr.h"
#include "include/pic.h"
#include "include/task.h"
#include "include/signal.h"
#include "include/vga.h"

#define KBD_DATA_PORT 0x60

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_CTRL   0x1D
#define SC_ALT    0x38
#define SC_F1     0x3B          /* F1..F4 leza kolejno: 0x3B..0x3E */
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

/* Kazda konsola wirtualna ma wlasna kolejke znakow i wlasnego
   wlasciciela. Gdy jakis proces czyta z niej klawiature (sys_read),
   znaki ladaja w kolejce zamiast u shella tej konsoli; po smierci
   wlasciciela konsola wraca do shella. */
#define KBD_BUF_SIZE 128

struct kbd_console {
    char buf[KBD_BUF_SIZE];
    uint32_t head, tail;
    int owner_id;                       /* proces czytajacy, -1 = shell */
    struct task *waiter;
    keyboard_char_handler_t on_char;    /* shell tej konsoli */
};

static struct kbd_console cons[CONSOLE_COUNT];
static keyboard_char_handler_t override;   /* pulpit zabiera wszystkie klawisze */
static int cons_ready;

static struct kbd_console *console(int n)
{
    if (!cons_ready) {                  /* -1 zamiast zera dla wlasciciela */
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

/* Pulpit dziala w trybie graficznym, gdzie konsole tekstowe nie maja
   sensu - przechwytuje wiec wszystkie klawisze, zanim trafia gdziekolwiek */
void keyboard_set_override(keyboard_char_handler_t handler)
{
    override = handler;
}

keyboard_char_handler_t keyboard_console_handler(void)
{
    return console(console_active())->on_char;
}

/* Dostarczenie znaku - kontekst IRQ (IF=0). Znak trafia na konsole,
   ktora akurat widac; ta decyduje, czy nalezy do procesu, czy do shella. */
static void deliver(char c)
{
    if (override) {                     /* pulpit */
        override(c);
        return;
    }

    struct kbd_console *k = console(console_active());
    if (k->owner_id >= 0 && !task_alive(k->owner_id))
        k->owner_id = -1;

    /* Ctrl+C nie jest znakiem do przeczytania, tylko sygnalem dla
       procesu trzymajacego konsole. Gdy konsoli nikt nie trzyma,
       przerywac nie ma kogo - shell jadra znak zignoruje. */
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

/* Wstrzykniecie znaku z innego zrodla niz PS/2 (klawiatura USB).
   Wolane z watku, wiec sami zamykamy sekcje krytyczna - dalsza droga
   znaku jest dokladnie ta sama, co przy przerwaniu klawiatury. */
void keyboard_inject(char c)
{
    uint32_t flags = irq_save();
    deliver(c);
    irq_restore(flags);
}

/* Przelaczenie konsoli z innego zrodla (Alt+F1..F4 na klawiaturze USB) */
void keyboard_inject_switch(int vc)
{
    if (override)                       /* pulpit trzyma caly ekran */
        return;
    uint32_t flags = irq_save();
    console_switch(vc);
    irq_restore(flags);
}

/* Blokujacy odczyt dla syscalla read - wola proces uzytkownika */
int keyboard_read(char *buf, uint32_t len)
{
    if (!len)
        return 0;

    uint32_t flags = irq_save();
    /* Proces czyta z WLASNEJ konsoli, nie z tej, na ktora akurat
       patrzymy - inaczej przelaczenie ekranu przekierowywaloby
       klawiature w srodku czyjegos odczytu */
    struct kbd_console *k = console(task_vc());
    k->owner_id = task_current_id();

    uint32_t got = 0;
    int interrupted = 0;
    while (!got) {
        while (k->tail != k->head && got < len)
            buf[got++] = k->buf[k->tail++ & (KBD_BUF_SIZE - 1)];
        if (got)
            break;
        /* Czekanie na klawisz przerywa sygnal (np. Ctrl+C): wracamy
           z bledem, zeby proces zdazyl wejsc w swoja procedure obslugi */
        if (task_signal_pending()) {
            interrupted = 1;
            break;
        }
        k->waiter = task_current();
        task_block();
    }

    if (k->waiter == task_current())    /* obudzil nas sygnal, nie klawisz */
        k->waiter = (void *)0;
    irq_restore(flags);
    return interrupted ? -1 : (int)got;
}

static void keyboard_irq(struct regs *r)
{
    (void)r;
    uint8_t scancode = inb(KBD_DATA_PORT);

    if (scancode == SC_EXTENDED) {      /* prefiks klawisza rozszerzonego */
        extended_next = 1;
        return;
    }
    if (extended_next) {
        extended_next = 0;
        if (!(scancode & SC_RELEASED)) {
            char c = extended_key(scancode);
            if (c)
                deliver(c);
        }
        return;
    }

    if (scancode == SC_ALT) {
        alt_pressed = 1;
        return;
    }
    if (scancode == (SC_ALT | SC_RELEASED)) {
        alt_pressed = 0;
        return;
    }
    /* Alt+F1..F4 przelacza konsole. W trybie graficznym nie ma czego
       przelaczac - pulpit trzyma caly ekran dla siebie. */
    if (alt_pressed && !override &&
        scancode >= SC_F1 && scancode < SC_F1 + CONSOLE_COUNT) {
        console_switch(scancode - SC_F1);
        return;
    }
    if (scancode == SC_CTRL) {
        ctrl_pressed = 1;
        return;
    }
    if (scancode == (SC_CTRL | SC_RELEASED)) {
        ctrl_pressed = 0;
        return;
    }
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
    if (ctrl_pressed && c == 'c')       /* pozostale kombinacje z Ctrl - jeszcze nie */
        c = KEY_INTR;
    if (c)
        deliver(c);
}

void keyboard_init(void)
{
    irq_install_handler(1, keyboard_irq);
    pic_unmask(1);
}
