#ifndef PROTONOS_SPINLOCK_H
#define PROTONOS_SPINLOCK_H

#include <stdint.h>
#include "io.h"

/* Spinlock
 *
 * The first thing a system needs once there is more than one CPU.
 * Turning off interrupts was enough while there was a single core:
 * nobody else could step into the middle of an operation. Now somebody
 * can - and really SIMULTANEOUSLY, not just apparently so.
 *
 * The xchg instruction is atomic by definition and on top of that acts
 * as a barrier for store ordering, so no extra fences are needed here:
 * whatever a core wrote before releasing the lock, the next core sees
 * after taking it.
 *
 * We wait by spinning, because the lock is held for a handful of
 * instructions - going to sleep would cost more than the wait itself.
 * For the same reason nothing long, and nothing that can put the task
 * to sleep, may happen while the lock is held.
 */

struct spinlock {
    volatile uint32_t taken;
};

#define SPINLOCK_FREE { 0 }

static inline void spin_lock(struct spinlock *z)
{
    uint32_t was;
    do {
        was = 1;
        __asm__ volatile("xchgl %0, %1"
                         : "+r"(was), "+m"(z->taken)
                         : : "memory");
        if (!was)
            break;
        /* Tell the CPU this is a wait loop: it saves power and stops the
           bus from being flooded with reads */
        __asm__ volatile("pause" ::: "memory");
    } while (1);
}

static inline void spin_unlock(struct spinlock *z)
{
    uint32_t zero = 0;
    __asm__ volatile("xchgl %0, %1"
                     : "+r"(zero), "+m"(z->taken)
                     : : "memory");
}

/* The lock taken together with interrupts turned off - which is what
 * almost every use inside the kernel looks like.
 *
 * The two guard against two different things. The lock guards against
 * ANOTHER CORE, one that really is running at the same time. Turning
 * interrupts off guards against OURSELVES: kernel structures are
 * touched both by threads and by interrupt handlers, so a timer that
 * stepped into the middle of a locked section would wait for a lock
 * held by the very code it interrupted - on the same core, the only
 * one that could ever release it.
 *
 * The order is not arbitrary: interrupts go off BEFORE the lock is
 * taken and come back AFTER it is released. The other way round would
 * leave a window in which an interrupt finds the lock already held.
 */
static inline irqflags_t spin_lock_irq(struct spinlock *z)
{
    irqflags_t flags = irq_save();
    spin_lock(z);
    return flags;
}

static inline void spin_unlock_irq(struct spinlock *z, irqflags_t flags)
{
    spin_unlock(z);
    irq_restore(flags);
}

#endif
