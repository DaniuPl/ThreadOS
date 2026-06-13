/* Potoki miedzyprocesowe (anonimowe)
 *
 * Kazdy potok to bufor kolowy plus liczniki otwartych koncow.
 * Producent pisze na swoj stdout (koniec zapisu), konsument czyta
 * ze swojego stdin (koniec odczytu); dane plyna przez bufor jadra.
 *
 * Synchronizacja jak przy klawiaturze: gdy konsument trafi na pusty
 * potok, a sa jeszcze pisarze - blokuje sie i czeka, az zapis go
 * obudzi. Gdy ostatni pisarz zamknie koniec, czytelnik dostaje EOF.
 * Symetrycznie pisarz czeka na pelnym buforze, az czytelnik zrobi
 * miejsce. Operacje biegna pod irq_save - krotkie sekcje krytyczne.
 */

#include "include/pipe.h"
#include "include/task.h"
#include "include/io.h"
#include "include/string.h"

#define PIPE_CAP   512
#define MAX_PIPES  4

struct pipe {
    int used;
    uint8_t buf[PIPE_CAP];
    uint32_t head;              /* zapisane bajty (monotonicznie) */
    uint32_t tail;              /* przeczytane bajty */
    int readers;
    int writers;
    struct task *read_waiter;
    struct task *write_waiter;
};

static struct pipe pipes[MAX_PIPES];

static uint32_t pipe_count(struct pipe *p) { return p->head - p->tail; }

int pipe_alloc(void)
{
    uint32_t flags = irq_save();
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].used) {
            memset(&pipes[i], 0, sizeof(pipes[i]));
            pipes[i].used = 1;
            pipes[i].readers = 1;
            pipes[i].writers = 1;
            irq_restore(flags);
            return i;
        }
    }
    irq_restore(flags);
    return -1;
}

int pipe_read(int idx, void *buf, uint32_t len)
{
    if (idx < 0 || idx >= MAX_PIPES)
        return -1;
    struct pipe *p = &pipes[idx];
    uint8_t *dst = buf;

    uint32_t flags = irq_save();
    /* Czekaj na dane, dopoki bufor pusty a ktos jeszcze moze pisac */
    while (pipe_count(p) == 0 && p->writers > 0) {
        p->read_waiter = task_current();
        task_block();
    }

    uint32_t got = 0;
    while (got < len && pipe_count(p) > 0)
        dst[got++] = p->buf[p->tail++ % PIPE_CAP];

    if (got && p->write_waiter) {       /* zrobilismy miejsce */
        task_unblock(p->write_waiter);
        p->write_waiter = (void *)0;
    }
    irq_restore(flags);
    return (int)got;                    /* 0 = EOF (brak pisarzy) */
}

int pipe_write(int idx, const void *buf, uint32_t len)
{
    if (idx < 0 || idx >= MAX_PIPES)
        return -1;
    struct pipe *p = &pipes[idx];
    const uint8_t *src = buf;

    uint32_t flags = irq_save();
    uint32_t put = 0;
    while (put < len) {
        if (p->readers == 0) {          /* potok przerwany */
            irq_restore(flags);
            return put ? (int)put : -1;
        }
        if (pipe_count(p) == PIPE_CAP) {  /* pelny - poczekaj na czytelnika */
            if (p->read_waiter) {
                task_unblock(p->read_waiter);
                p->read_waiter = (void *)0;
            }
            p->write_waiter = task_current();
            task_block();
            continue;
        }
        p->buf[p->head++ % PIPE_CAP] = src[put++];
    }

    if (p->read_waiter) {               /* obudz czytelnika - sa dane */
        task_unblock(p->read_waiter);
        p->read_waiter = (void *)0;
    }
    irq_restore(flags);
    return (int)put;
}

static void pipe_maybe_free(struct pipe *p)
{
    if (p->readers == 0 && p->writers == 0)
        p->used = 0;
}

void pipe_close_reader(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES)
        return;
    struct pipe *p = &pipes[idx];
    uint32_t flags = irq_save();
    if (p->readers > 0)
        p->readers--;
    if (p->write_waiter) {              /* pisarz nie ma juz po co czekac */
        task_unblock(p->write_waiter);
        p->write_waiter = (void *)0;
    }
    pipe_maybe_free(p);
    irq_restore(flags);
}

void pipe_close_writer(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES)
        return;
    struct pipe *p = &pipes[idx];
    uint32_t flags = irq_save();
    if (p->writers > 0)
        p->writers--;
    if (p->read_waiter) {               /* obudz - niech zobaczy EOF */
        task_unblock(p->read_waiter);
        p->read_waiter = (void *)0;
    }
    pipe_maybe_free(p);
    irq_restore(flags);
}
