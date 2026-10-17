/* Mutexes, on top of the kernel's counting semaphores
 *
 * A mutex is a semaphore that starts with one token: locking takes it,
 * unlocking gives it back, and whoever comes in between waits. Found by
 * key, like the semaphore under it - two processes asking for the same
 * key share the same lock.
 */

#ifndef THREADOS_USEM_H
#define THREADOS_USEM_H

#include "uapi.h"

static inline int mutex_create(int key)
{
    return sys_sem_get(key, 1);
}

static inline int mutex_lock(int m)
{
    return sys_sem_wait(m);
}

static inline int mutex_unlock(int m)
{
    return sys_sem_post(m);
}

#endif
