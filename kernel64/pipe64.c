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

struct pipe {
    int uzyty;
    uint8_t buf[PIPE_BUF];
    uint32_t head, tail;
    int reader, writer;
};

static struct pipe pipes[PIPE_MAX];

static uint32_t inner_count(const struct pipe *p)
{
    return p->head - p->tail;
}

int pipe64_new(void)
{
    irqflags_t flags = irq_save();
    int idx = -1;
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!pipes[i].uzyty) {
            pipes[i].uzyty = 1;
            pipes[i].head = pipes[i].tail = 0;
            pipes[i].reader = 1;
            pipes[i].writer = 1;
            idx = i;
            break;
        }
    }
    irq_restore(flags);
    return idx;
}

void pipe64_ref(int idx, int reader_task)
{
    if (idx < 0 || idx >= PIPE_MAX)
        return;
    irqflags_t flags = irq_save();
    if (reader_task)
        pipes[idx].reader++;
    else
        pipes[idx].writer++;
    irq_restore(flags);
}

void pipe64_unref(int idx, int reader_task)
{
    if (idx < 0 || idx >= PIPE_MAX)
        return;
    irqflags_t flags = irq_save();
    struct pipe *p = &pipes[idx];
    if (reader_task) {
        if (p->reader)
            p->reader--;
    } else if (p->writer) {
        p->writer--;
    }
    if (!p->reader && !p->writer)
        p->uzyty = 0;
    irq_restore(flags);
}

int pipe64_read(int idx, char *buf, uint32_t len)
{
    if (idx < 0 || idx >= PIPE_MAX || !len)
        return -1;
    struct pipe *p = &pipes[idx];

    for (;;) {
        irqflags_t flags = irq_save();
        uint32_t taken_n = 0;
        while (inner_count(p) && taken_n < len)
            buf[taken_n++] = (char)p->buf[p->tail++ % PIPE_BUF];
        int end = !p->writer;
        irq_restore(flags);

        if (taken_n)
            return (int)taken_n;
        if (end)
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
    struct pipe *p = &pipes[idx];
    uint32_t given_back_n = 0;

    while (given_back_n < len) {
        irqflags_t flags = irq_save();
        if (!p->reader) {         /* nikt tego juz nie przeczyta */
            irq_restore(flags);
            return given_back_n ? (int)given_back_n : -1;
        }
        while (inner_count(p) < PIPE_BUF && given_back_n < len)
            p->buf[p->head++ % PIPE_BUF] = (uint8_t)buf[given_back_n++];
        irq_restore(flags);

        if (given_back_n == len)
            break;
        if (task64_signal_pending())
            return given_back_n ? (int)given_back_n : -1;
        task64_sleep(10);            /* pelny - czekamy na czytajacego */
    }
    return (int)given_back_n;
}
