/* Inter-process (anonymous) pipes
 *
 * Every pipe is a ring buffer plus counters of open ends. The producer
 * writes to its stdout (the write end), the consumer reads from its
 * stdin (the read end); the data flows through a kernel buffer.
 *
 * The synchronisation is as with the keyboard: when a consumer finds
 * an empty pipe while there are still writers - it blocks and waits
 * for a write to wake it. When the last writer closes its end, the
 * reader gets EOF. Symmetrically a writer waits on a full buffer until
 * the reader makes room. The operations run under irq_save - short
 * critical sections.
 */

#include "include/pipe.h"
#include "include/task.h"
#include "include/io.h"
#include "include/string.h"

#define PIPE_CAP   512
#define MAX_PIPES  8

struct pipe {
    int used;
    uint8_t buf[PIPE_CAP];
    uint32_t head;              /* bytes written (monotonically) */
    uint32_t tail;              /* bytes read */
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
    /* Wait for data while the buffer is empty and somebody can still write */
    while (pipe_count(p) == 0 && p->writers > 0) {
        p->read_waiter = task_current();
        task_block();
    }

    uint32_t got = 0;
    while (got < len && pipe_count(p) > 0)
        dst[got++] = p->buf[p->tail++ % PIPE_CAP];

    if (got && p->write_waiter) {       /* we made room */
        task_unblock(p->write_waiter);
        p->write_waiter = (void *)0;
    }
    irq_restore(flags);
    return (int)got;                    /* 0 = EOF (no writers) */
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
        if (p->readers == 0) {          /* a broken pipe */
            irq_restore(flags);
            return put ? (int)put : -1;
        }
        if (pipe_count(p) == PIPE_CAP) {  /* full - wait for the reader */
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

    if (p->read_waiter) {               /* wake the reader - there is data */
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

/* Adds an owner to a pipe end - after fork both sides hold the same
   end and only closing both gives the other side EOF. */
void pipe_add_reader(int idx)
{
    if (idx >= 0 && idx < MAX_PIPES && pipes[idx].used)
        pipes[idx].readers++;
}

void pipe_add_writer(int idx)
{
    if (idx >= 0 && idx < MAX_PIPES && pipes[idx].used)
        pipes[idx].writers++;
}

void pipe_close_reader(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES)
        return;
    struct pipe *p = &pipes[idx];
    uint32_t flags = irq_save();
    if (p->readers > 0)
        p->readers--;
    if (p->write_waiter) {              /* the writer has nothing left to wait for */
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
    if (p->read_waiter) {               /* wake it - let it see the EOF */
        task_unblock(p->read_waiter);
        p->read_waiter = (void *)0;
    }
    pipe_maybe_free(p);
    irq_restore(flags);
}
