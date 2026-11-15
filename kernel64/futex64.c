/* Futexes: a thread waiting on a word of its own memory
 *
 * See futex64.h for what they are. Here: one table of waiters for the
 * whole machine, each entry a sleeping task and the word it sleeps on.
 * A wake walks the table; the table is small and a wake is rare next
 * to the lock operations that never reach the kernel at all.
 *
 * A wake must not fall between a waiter's look at the word and its
 * sleep, or the waiter sleeps through it. The look and the filing
 * happen under this file's lock, a wake finds the waiter only under the
 * same lock, and the sleep itself (task64_block) refuses to start once
 * task64_wake has marked the task - so whichever order the two cores
 * come in, the waiter either sees the new value or is woken.
 */

#include <stdint.h>
#include "futex64.h"
#include "task64.h"
#include "paging64.h"
#include "../kernel/include/spinlock.h"
#include "../kernel/include/pit.h"

#define WAITERS 128

struct waiter {
    struct task64 *task;         /* 0 = a free entry */
    uint64_t space;
    uint64_t addr;
    int woken;
};

static struct waiter waiters[WAITERS];
static struct spinlock lock = SPINLOCK_FREE;

/* The word has to be there to be read under a spinlock: a page that
   went out to disk is fetched back first, outside it */
static int word_ready(uint64_t space, uint64_t addr)
{
    if (paging64_user_range(addr, 4))
        return 1;
    return paging64_swap_in(space, addr) && paging64_user_range(addr, 4);
}

int futex64_wait(uint64_t addr, uint32_t val, uint32_t ms)
{
    uint64_t space = paging64_current_space();
    if (addr & 3)
        return -FUTEX64_EINVAL;
    if (!word_ready(space, addr))
        return -FUTEX64_EFAULT;

    irqflags_t flags = spin_lock_irq(&lock);
    if (!paging64_user_range(addr, 4)) {     /* went out again meanwhile */
        spin_unlock_irq(&lock, flags);
        return -FUTEX64_EAGAIN;
    }
    if (*(volatile uint32_t *)(uintptr_t)addr != val) {
        spin_unlock_irq(&lock, flags);
        return -FUTEX64_EAGAIN;
    }
    struct waiter *w = 0;
    for (int i = 0; i < WAITERS; i++)
        if (!waiters[i].task) {
            w = &waiters[i];
            break;
        }
    if (!w) {
        /* No room: the caller looks at the word again and comes back,
           which is a busy wait, but a correct one */
        spin_unlock_irq(&lock, flags);
        return -FUTEX64_EAGAIN;
    }
    w->task = task64_self();
    w->space = space;
    w->addr = addr;
    w->woken = 0;
    task64_block_prepare();
    spin_unlock_irq(&lock, flags);

    uint32_t start = pit_ticks();
    task64_block(ms ? (ms + 9) / 10 : 0);    /* one tick is 10 ms */

    flags = spin_lock_irq(&lock);
    int woken = w->woken;
    w->task = 0;
    spin_unlock_irq(&lock, flags);

    if (woken)
        return 0;
    if (task64_signal_pending())
        return -FUTEX64_EINTR;
    if (ms && pit_ticks() - start >= (ms + 9) / 10)
        return -FUTEX64_ETIMEDOUT;
    return 0;                                 /* a spurious wake is allowed */
}

int futex64_wake(uint64_t space, uint64_t addr, int n)
{
    int woke = 0;
    irqflags_t flags = spin_lock_irq(&lock);
    for (int i = 0; i < WAITERS && woke < n; i++) {
        struct waiter *w = &waiters[i];
        if (w->task && !w->woken && w->space == space && w->addr == addr) {
            w->woken = 1;
            task64_wake(w->task);
            woke++;
        }
    }
    spin_unlock_irq(&lock, flags);
    return woke;
}

int futex64_requeue(uint64_t addr, int n, int m, uint64_t addr2)
{
    uint64_t space = paging64_current_space();
    if ((addr | addr2) & 3)
        return -FUTEX64_EINVAL;
    int woke = 0, moved = 0;
    irqflags_t flags = spin_lock_irq(&lock);
    for (int i = 0; i < WAITERS; i++) {
        struct waiter *w = &waiters[i];
        if (!w->task || w->woken || w->space != space || w->addr != addr)
            continue;
        if (woke < n) {
            w->woken = 1;
            task64_wake(w->task);
            woke++;
        } else if (moved < m) {
            w->addr = addr2;
            moved++;
        }
    }
    spin_unlock_irq(&lock, flags);
    return woke;
}
