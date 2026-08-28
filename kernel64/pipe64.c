/* Potoki
 *
 * Pierscien w pamieci jadra plus dwa liczniki: ilu jest czytajacych
 * i ilu piszacych. Te liczniki sa wazniejsze od samego bufora, bo to
 * one mowia, kiedy skonczyc.
 *
 * Czekanie musi dac sie przerwac sygnalem. Bez tego program czekajacy
 * na dane od drugiego, ktory sie zawiesil, byiby nie do zatrzymania -
 * a to dokladnie ten rodzaj zawieszenia, ktory juz raz w tym jadrze
 * byl: sygnal dostarcza sie tylko przy powrocie do pierscienia 3,
 * wiec petla czekajaca w jadrze musi sprawdzac go sama.
 */

#include "pipe64.h"
#include "task64.h"
#include "../kernel/include/io.h"

#define PIPE_MAX 8
#define PIPE_BUF 2048

struct potok {
    int uzyty;
    uint8_t buf[PIPE_BUF];
    uint32_t head, tail;
    int czytajacy, piszacy;
};

static struct potok potoki[PIPE_MAX];

static uint32_t ile_w_srodku(const struct potok *p)
{
    return p->head - p->tail;
}

int pipe64_new(void)
{
    irqflags_t flags = irq_save();
    int idx = -1;
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!potoki[i].uzyty) {
            potoki[i].uzyty = 1;
            potoki[i].head = potoki[i].tail = 0;
            potoki[i].czytajacy = 1;
            potoki[i].piszacy = 1;
            idx = i;
            break;
        }
    }
    irq_restore(flags);
    return idx;
}

void pipe64_ref(int idx, int czytelnik)
{
    if (idx < 0 || idx >= PIPE_MAX)
        return;
    irqflags_t flags = irq_save();
    if (czytelnik)
        potoki[idx].czytajacy++;
    else
        potoki[idx].piszacy++;
    irq_restore(flags);
}

void pipe64_unref(int idx, int czytelnik)
{
    if (idx < 0 || idx >= PIPE_MAX)
        return;
    irqflags_t flags = irq_save();
    struct potok *p = &potoki[idx];
    if (czytelnik) {
        if (p->czytajacy)
            p->czytajacy--;
    } else if (p->piszacy) {
        p->piszacy--;
    }
    if (!p->czytajacy && !p->piszacy)
        p->uzyty = 0;
    irq_restore(flags);
}

int pipe64_read(int idx, char *buf, uint32_t len)
{
    if (idx < 0 || idx >= PIPE_MAX || !len)
        return -1;
    struct potok *p = &potoki[idx];

    for (;;) {
        irqflags_t flags = irq_save();
        uint32_t wzieto = 0;
        while (ile_w_srodku(p) && wzieto < len)
            buf[wzieto++] = (char)p->buf[p->tail++ % PIPE_BUF];
        int koniec = !p->piszacy;
        irq_restore(flags);

        if (wzieto)
            return (int)wzieto;
        if (koniec)
            return 0;                /* nie ma juz kto pisac - koniec danych */
        if (task64_signal_pending())
            return -1;
        task64_sleep(10);
    }
}

int pipe64_write(int idx, const char *buf, uint32_t len)
{
    if (idx < 0 || idx >= PIPE_MAX || !len)
        return -1;
    struct potok *p = &potoki[idx];
    uint32_t oddano = 0;

    while (oddano < len) {
        irqflags_t flags = irq_save();
        if (!p->czytajacy) {         /* nikt tego juz nie przeczyta */
            irq_restore(flags);
            return oddano ? (int)oddano : -1;
        }
        while (ile_w_srodku(p) < PIPE_BUF && oddano < len)
            p->buf[p->head++ % PIPE_BUF] = (uint8_t)buf[oddano++];
        irq_restore(flags);

        if (oddano == len)
            break;
        if (task64_signal_pending())
            return oddano ? (int)oddano : -1;
        task64_sleep(10);            /* pelny - czekamy na czytajacego */
    }
    return (int)oddano;
}
