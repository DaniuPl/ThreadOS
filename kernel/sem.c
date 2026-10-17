/* Counting semaphores - threads and processes waiting for one another
 *
 * A semaphore lives in the kernel and is found by a key, as shared
 * memory is: the first sem_get(key, n) makes it with n tokens, every
 * later one with the same key gets the same semaphore. A mutex is a
 * semaphore that starts with one.
 *
 * Waiting differs between the two kernels and nothing else does. The
 * 32-bit one can put a task to sleep until somebody wakes it, so a
 * waiter goes into a FIFO queue and sem_post wakes the longest-waiting
 * one - the same way pipe.c waits. The 64-bit one has no such call
 * yet, so a waiter sleeps a tick at a time and looks again - the way
 * pipe64.c waits. Either way a process being killed stops waiting: a
 * semaphore must not be the thing that keeps a dead process alive.
 *
 * Ported from the thread-synchronisation work on an older branch
 * (claude/custom-os-skeleton-wow4s7, 9743c31), which had only the
 * single-core 32-bit kernel to run on.
 */

#include <stdint.h>
#include "include/sem.h"
#include "include/io.h"
#include "include/spinlock.h"

#ifdef __x86_64__
#include "../kernel64/task64.h"
#else
#include "include/task.h"
#endif

#define MAX_SEM     16              /* different semaphores at once */
#define SEM_WAITERS 8               /* the queue on one of them */

struct sem {
    int used;
    int key;
    int count;                      /* tokens free */
#ifndef __x86_64__
    struct task *waiters[SEM_WAITERS];  /* FIFO */
    int nwait;
#endif
};

static struct sem sems[MAX_SEM];
static struct spinlock lock = SPINLOCK_FREE;

static struct sem *by_id(int id)
{
    if (id < 0 || id >= MAX_SEM || !sems[id].used)
        return (struct sem *)0;
    return &sems[id];
}

int sem_get(int key, int initial)
{
    if (initial < 0)
        return -1;
    irqflags_t flags = spin_lock_irq(&lock);
    int id = -1;
    for (int i = 0; i < MAX_SEM; i++)
        if (sems[i].used && sems[i].key == key) {
            id = i;                     /* it is there already */
            break;
        }
    if (id < 0)
        for (int i = 0; i < MAX_SEM; i++) {
            if (sems[i].used)
                continue;
            sems[i] = (struct sem){ .used = 1, .key = key, .count = initial };
            id = i;
            break;
        }
    spin_unlock_irq(&lock, flags);
    return id;
}

#ifndef __x86_64__
/* Takes a task out of the queue, if it is there, keeping the order */
static void unqueue(struct sem *s, struct task *t)
{
    for (int i = 0; i < s->nwait; i++) {
        if (s->waiters[i] != t)
            continue;
        for (int j = i + 1; j < s->nwait; j++)
            s->waiters[j - 1] = s->waiters[j];
        s->nwait--;
        return;
    }
}

/* With interrupts off throughout, as pipe.c does it: the 32-bit
   kernel's critical section, so a post cannot fall between the check
   and the sleep. task_block() lets interrupts back in only while this
   task is not running. */
int sem_wait(int id)
{
    uint32_t flags = irq_save();
    struct sem *s = by_id(id);
    if (!s) {
        irq_restore(flags);
        return -1;
    }
    while (s->count == 0) {
        if (task_killed() || s->nwait >= SEM_WAITERS) {
            irq_restore(flags);
            return -1;
        }
        s->waiters[s->nwait++] = task_current();
        task_block();                   /* sem_post or a kill wakes us */
        unqueue(s, task_current());
    }
    s->count--;
    irq_restore(flags);
    return 0;
}

int sem_post(int id)
{
    uint32_t flags = irq_save();
    struct sem *s = by_id(id);
    if (!s) {
        irq_restore(flags);
        return -1;
    }
    s->count++;
    if (s->nwait > 0)
        task_unblock(s->waiters[0]);    /* the longest waiter */
    irq_restore(flags);
    return 0;
}

#else

int sem_wait(int id)
{
    for (;;) {
        irqflags_t flags = spin_lock_irq(&lock);
        struct sem *s = by_id(id);
        if (!s) {
            spin_unlock_irq(&lock, flags);
            return -1;
        }
        if (s->count > 0) {
            s->count--;
            spin_unlock_irq(&lock, flags);
            return 0;
        }
        spin_unlock_irq(&lock, flags);
        if (task64_signal_pending())
            return -1;                  /* let the signal be delivered */
        task64_sleep(1);
    }
}

int sem_post(int id)
{
    irqflags_t flags = spin_lock_irq(&lock);
    struct sem *s = by_id(id);
    if (s)
        s->count++;
    spin_unlock_irq(&lock, flags);
    return s ? 0 : -1;
}

#endif
