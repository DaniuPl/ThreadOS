/* One thread runs the model: a lock is nothing to take */
#ifndef HW_IWL_SPINLOCK_H
#define HW_IWL_SPINLOCK_H
struct spinlock { int held; };
#define SPINLOCK_FREE { 0 }
static inline void spin_lock(struct spinlock *z) { z->held = 1; }
static inline void spin_unlock(struct spinlock *z) { z->held = 0; }
#endif
