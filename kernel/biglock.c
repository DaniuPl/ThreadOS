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
#include "include/tlb.h"

static struct spinlock lock = SPINLOCK_FREE;
static volatile int owner = -1;
static volatile int depth;

int biglock_held(void)
{
    return owner == smp_core();
}

/* Waiting for the lock, we keep answering.
 *
 * A core comes here from an interrupt gate, so with interrupts off -
 * and a core with interrupts off hears nothing. That is fine while the
 * only thing anybody can be waiting for is this lock: whoever holds it
 * is running and will let go.
 *
 * It stopped being fine the moment the holder could wait for US. A
 * changed mapping has to reach every core that might be holding the
 * old one, and it is not honoured until they say so - so a core that
 * goes deaf while it waits here is a core the holder waits for
 * forever. Hence: one attempt, an answer, another attempt. */
static void wait_for_lock(void)
{
    while (!spin_trylock(&lock)) {
        tlb_service();
        __asm__ volatile("pause" ::: "memory");
    }
}

void biglock_enter(void)
{
    int nr = smp_core();
    if (owner == nr) {
        depth++;
        return;
    }
    wait_for_lock();
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
    int count = depth;
    depth = 0;
    owner = -1;
    spin_unlock(&lock);
    return count;
}

void biglock_restore(int count)
{
    if (count <= 0)
        return;
    wait_for_lock();
    owner = smp_core();
    depth = count;
}
