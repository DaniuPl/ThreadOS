#ifndef THREADOS_FUTEX64_H
#define THREADOS_FUTEX64_H

#include <stdint.h>

/* Futexes: a thread waiting on a word of its own memory
 *
 * The waiting is decided in ring 3: a C library's mutex is a word that
 * threads change with atomic instructions, and the kernel is asked only
 * when a thread has to wait for it, or has to wake one that waits. A
 * waiter is filed under its address space and the word's address, so
 * only threads of one program meet on a word.
 *
 * The results are Linux's: 0, or minus an errno - the one caller is the
 * C library, which passes them straight on.
 */

#define FUTEX64_EAGAIN    11     /* the word no longer held the value */
#define FUTEX64_EINTR      4     /* a signal came first */
#define FUTEX64_EFAULT    14
#define FUTEX64_EINVAL    22
#define FUTEX64_ETIMEDOUT 110

/* Sleeps while *addr == val, at most 'ms' milliseconds (0 = no limit) */
int futex64_wait(uint64_t addr, uint32_t val, uint32_t ms);
/* Wakes up to n waiters on the word; how many woke */
int futex64_wake(uint64_t space, uint64_t addr, int n);
/* Wakes up to n, and moves up to m of the rest to wait on addr2 */
int futex64_requeue(uint64_t addr, int n, int m, uint64_t addr2);

#endif
