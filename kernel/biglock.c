/* The big kernel lock - one lock for all kernel code
 *
 * Recursive, because it has to be: an interrupt can land in the middle
 * of a system call, and a core that already holds the lock cannot wait
 * for itself. The depth counter and the owner number are protected by
 * the lock itself - only whoever holds it writes to them.
 *
 * Reading 'owner' without the lock is safe and is meant to be: when
 * another core holds it we read its number, which is not ours, and go
 * and wait. When nobody holds it we read -1. In both cases the answer
 * to "is that me" is correct.
 */

#include "include/biglock.h"
#include "include/spinlock.h"
#include "include/smp.h"

static struct spinlock lock = SPINLOCK_FREE;
static volatile int owner = -1;
static volatile int depth;

int biglock_held(void)
{
    return owner == smp_core();
}

void biglock_enter(void)
{
    int nr = smp_core();
    if (owner == nr) {
        depth++;
        return;
    }
    spin_lock(&lock);
    owner = nr;
    depth = 1;
}

void biglock_leave(void)
{
    if (owner != smp_core())
        return;
    if (--depth > 0)
        return;
    owner = -1;
    spin_unlock(&lock);
}

int biglock_release_all(void)
{
    if (owner != smp_core())
        return 0;
    int ile = depth;
    depth = 0;
    owner = -1;
    spin_unlock(&lock);
    return ile;
}

void biglock_restore(int ile)
{
    if (ile <= 0)
        return;
    spin_lock(&lock);
    owner = smp_core();
    depth = ile;
}
