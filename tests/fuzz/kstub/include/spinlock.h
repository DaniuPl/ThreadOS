/* A lock is not needed on the host: the harness has one thread */
#ifndef STUB_SPINLOCK_H
#define STUB_SPINLOCK_H
#include "io.h"
struct spinlock { int held; };
#define SPINLOCK_FREE { 0 }
static inline void spin_lock(struct spinlock *z) { (void)z; }
static inline void spin_unlock(struct spinlock *z) { (void)z; }
static inline irqflags_t spin_lock_irq(struct spinlock *z)
{
    (void)z;
    return 0;
}
static inline void spin_unlock_irq(struct spinlock *z, irqflags_t f)
{
    (void)z;
    (void)f;
}
#endif
