#ifndef THREADOS_SEM_H
#define THREADOS_SEM_H

/* Counting semaphores, shared by key (a mutex is a semaphore of one)
 *
 * The same key gives the same semaphore, the way it gives the same
 * shared memory - so threads of one process and separate processes can
 * meet on it. */

/* The semaphore with this key; created with 'initial' tokens when it
   does not exist yet. Its id, or -1 when there is no room. */
int sem_get(int key, int initial);

/* Takes a token, waiting while there is none. 0, or -1 for a bad id, a
   full queue of waiters, or a process that is being killed. */
int sem_wait(int id);

/* Gives a token back and wakes the longest waiter. 0 or -1. */
int sem_post(int id);

#endif
