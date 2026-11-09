#ifndef THREADOS_SPINLOCK_H
#define THREADOS_SPINLOCK_H

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

#if defined(__aarch64__)
/* ARM has no xchg that is a barrier by itself. The compiler's atomics
   make the exclusive load and store pair out of these (ldaxr/stlxr),
   with acquire on the way in and release on the way out - the same
   promise xchg makes on x86: what the last holder wrote before letting
   go is seen by the next one. They need the MMU on: on Device memory,
   the exclusive pair never succeeds. While waiting, only read - a
   store attempt per loop would steal the line back and forth. */
static inline void spin_lock(struct spinlock *z)
{
    while (__atomic_exchange_n(&z->taken, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(&z->taken, __ATOMIC_RELAXED))
            __asm__ volatile("yield" ::: "memory");
}

static inline int spin_trylock(struct spinlock *z)
{
    return !__atomic_exchange_n(&z->taken, 1, __ATOMIC_ACQUIRE);
}

static inline void spin_unlock(struct spinlock *z)
{
    __atomic_store_n(&z->taken, 0, __ATOMIC_RELEASE);
}
#else
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

/* One attempt, no waiting: 1 = taken. For a caller that has something
   to do while it waits - answering another core, for instance. */
static inline int spin_trylock(struct spinlock *z)
{
    uint32_t was = 1;
    __asm__ volatile("xchgl %0, %1"
                     : "+r"(was), "+m"(z->taken)
                     : : "memory");
    return !was;
}

static inline void spin_unlock(struct spinlock *z)
{
    uint32_t zero = 0;
    __asm__ volatile("xchgl %0, %1"
                     : "+r"(zero), "+m"(z->taken)
                     : : "memory");
}
#endif

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
