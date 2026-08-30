/* Klawiatura PS/2 w trybie dlugim
 *
 * Tlumaczenie scancode'ow jest wspolne z jadrem 32-bitowym
 * (kernel/kbdmap.c) - tutaj zostaje tylko to, co zalezy od systemu:
 * odebranie przerwania i zdecydowanie, dokad trafi gotowy znak.
 *
 * Konsol jest cztery i kazda ma wlasna kolejke znakow. Klawisz trafia
 * do tej, na ktora akurat patrzymy, a czyta z niej ten, kto na niej
 * pisze - program uruchomiony na drugiej konsoli czeka wiec na swoje
 * klawisze i nie zabiera cudzych. Alt+F1..F4 przelacza obraz.
 *
 * Przerwanie tylko wklada znak do kolejki: w jego obsludze nie ma czego
 * drukowac ani na co czekac.
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

/* Jedna kolejka na konsole. Kto dostanie Ctrl+C, tez jest sprawa
   konsoli: shell ustawia to na czas, w ktorym czeka na swoje dziecko -
   poza tym czasem przerywac nie ma kogo. */
struct queue {
    char buf[KBD_BUF_SIZE];
    uint32_t head, tail;
    int intr_target;
};
static struct queue queues[CONSOLE_COUNT];
static int queues_ready;

static struct queue *console(int n)
{
    if (!queues_ready) {              /* -1 zamiast zera dla celu Ctrl+C */
        for (int i = 0; i < CONSOLE_COUNT; i++)
            queues[i].intr_target = -1;
        queues_ready = 1;
    }
    if (n < 0 || n >= CONSOLE_COUNT)
        n = 0;
    return &queues[n];
}

/* Pulpit dziala w trybie graficznym, gdzie konsola tekstowa nie ma
   sensu - przechwytuje wiec wszystkie klawisze, zanim trafia do
   kolejki, z ktorej czyta shell. */
static void (*override)(char c);

void keyboard64_set_override(void (*handler)(char c))
{
    override = handler;
}

/* Cel Ctrl+C ustawia shell tej konsoli, na ktorej sam pisze */
void keyboard64_set_intr_target(int id)
{
    console(task64_vc())->intr_target = id;
}

/* Doreczenie gotowego znaku. Wolane z przerwania klawiatury i z watku
   klawiatury USB - droga znaku jest w obu przypadkach ta sama. */
static void dorecz(char c)
{
    if (override) {                      /* pulpit bierze wszystko */
        override(c);
        return;
    }

    /* Klawisz idzie na konsole, na ktora patrzymy - to ona jest pod
       palcami piszacego */
    struct queue *k = console(console_active());

    /* Ctrl+C nie jest znakiem do przeczytania, tylko zadaniem
       przerwania programu, ktory trzyma konsole */
    if (c == KEY_INTR) {
        /* Nie do shella, ktory czeka, tylko do tego, co na koncu
           lancucha naprawde dziala */
        if (k->intr_target >= 0)
            task64_signal(task64_foreground(k->intr_target), SIGINT);
        return;
    }

    if (k->head - k->tail < KBD_BUF_SIZE)
        k->buf[k->head++ & (KBD_BUF_SIZE - 1)] = c;
}

/* Przelaczenie obrazu na inna konsole. Pulpit trzyma caly ekran, wiec
   przy nim przelaczac nie ma czego. */
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

/* Znak z innego zrodla niz PS/2 (klawiatura USB). Wolane z watku, wiec
   sekcje krytyczna zamykamy sami. */
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

/* Czeka na klawisz. Odpytujemy z uspieniem zamiast blokowac zadanie:
   przy jednym czytelniku roznica jest niewidoczna, a kodu o polowe
   mniej - budzenie z przerwania przyjdzie razem z drugim czytelnikiem. */
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

/* Odczyt dla programu w ring 3.
 *
 * Blokuje do pierwszego znaku, potem oddaje wszystko, co zdazylo sie
 * uzbierac - tak jak kazde sensowne wejscie: czekamy na cokolwiek,
 * a nie na pelny bufor.
 *
 * Sygnal przerywa czekanie i wraca bledem. Bez tego Ctrl+C nie mialby
 * jak dosiegnac programu, ktory wlasnie czeka na klawisz - a to jest
 * dokladnie ten program, ktoremu Ctrl+C jest najczesciej potrzebny. */
int keyboard64_read(char *out, uint32_t len)
{
    if (!len)
        return 0;

    for (;;) {
        irqflags_t flags = irq_save();
        /* Program czyta z WLASNEJ konsoli, nie z tej, na ktora akurat
           patrzymy - inaczej przelaczenie ekranu przekierowywaloby
           klawiature w srodku czyjegos odczytu */
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
