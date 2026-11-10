/* Counting semaphores - threads and processes waiting for one another
 *
 * A semaphore lives in the kernel and is found by a key, as shared
 * memory is: the first sem_get(key, n) makes it with n tokens, every
 * later one with the same key gets the same semaphore. A mutex is a
 * semaphore that starts with one.
 *
 * The scheduler has no call that puts a task to sleep until somebody
 * wakes it, so a waiter sleeps a tick at a time and looks again - the
 * way pipe64.c waits. A process being killed stops waiting: a
 * semaphore must not be the thing that keeps a dead process alive.
 *
 * Ported from the thread-synchronisation work on an older branch
 * (claude/custom-os-skeleton-wow4s7, 9743c31).
 */

#include <stdint.h>
#include "include/sem.h"
#include "include/io.h"
#include "include/spinlock.h"

#include "../kernel64/task64.h"

#define MAX_SEM     16              /* different semaphores at once */

struct sem {
    int used;
    int key;
    int count;                      /* tokens free */
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

