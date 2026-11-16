/* One thread on the host: a lock has nobody to keep out, but taking
   one twice is a deadlock on the machine, and is caught here */
#ifndef HW_XHCI_SPINLOCK_H
#define HW_XHCI_SPINLOCK_H
#include <stdio.h>
#include <stdlib.h>
#include "io.h"
struct spinlock { int taken; };
#define SPINLOCK_FREE { 0 }
static inline irqflags_t spin_lock_irq(struct spinlock *z)
{
    if (z->taken) {
        printf("FAIL: a spinlock taken twice - a deadlock on the machine\n");
        abort();
    }
    z->taken = 1;
    return 0;
}
static inline void spin_unlock_irq(struct spinlock *z, irqflags_t f)
{
    (void)f;
    z->taken = 0;
}
#endif
