/* A lock is not needed on the host: the harness has one thread */
#ifndef STUB_SPINLOCK_H
#define STUB_SPINLOCK_H
struct spinlock { int held; };
#define SPINLOCK_FREE { 0 }
static inline void spin_lock(struct spinlock *z) { (void)z; }
static inline void spin_unlock(struct spinlock *z) { (void)z; }
#endif
