#ifndef PROTONOS_BIGLOCK_H
#define PROTONOS_BIGLOCK_H

/* The big kernel lock
 *
 * Secondary cores can already run kernel threads, but only the ones
 * somebody explicitly allowed there - because the rest of the kernel
 * was written for a single core. To let PROGRAMS in, the same problem
 * has to be settled for the whole kernel at once.
 *
 * The answer is the one every system moving to many cores once gave:
 * a single lock around the entire kernel. User code then really does
 * run in parallel - and that is where programs spend most of their
 * time - while kernel code runs as before, one core at a time. This is
 * not the destination, it is the correct road to it: every subsystem
 * that gets a lock of its own can be lifted out from under this one,
 * and the system keeps working the whole way.
 *
 * The lock is RECURSIVE. A core that already holds it takes it again
 * without waiting - otherwise an interrupt landing in the middle of a
 * system call would wait for a lock held by the very code it
 * interrupted.
 *
 * The rule everything rests on: a task holds this lock for as long as
 * it runs kernel code, and gives it up FOR THE DURATION OF A CONTEXT
 * SWITCH. Idle tasks never take it - a core sitting in hlt with the
 * lock in its hand would stop the whole system.
 */

/* Entering kernel code (or already inside it - then just the count) */
void biglock_enter(void);

/* Leaving; the lock is only released on the outermost exit */
void biglock_leave(void);

/* Does this core hold the lock */
int biglock_held(void);

/* Give the lock up COMPLETELY and return how many times it was taken -
   for use around a context switch, where a task has to let go so that
   another one can enter the kernel. */
int biglock_release_all(void);

/* Take it back to the same depth */
void biglock_restore(int depth);

#endif
