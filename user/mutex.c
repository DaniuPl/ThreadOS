/* mutex - a mutex repairs a race between two threads
 *
 * Two threads of one process bump a shared counter. The critical
 * section is stretched on purpose - read, sleep, write - so that without
 * protection a switch in the middle loses updates: both threads read
 * the same value and both write it back plus one.
 *
 * Round 1, no mutex: the result falls short of 2*N.
 * Round 2, with the mutex: exactly 2*N.
 *
 * Both halves are printed and both are checked: a counter that came out
 * right without the mutex would mean the race was never there, and the
 * mutex then proved nothing.
 *
 * Built for both kernels from this one source (uapi.h picks the side).
 */

#include "uapi.h"
#include "usem.h"

#define N     10                    /* bumps per thread */
#define MKEY  0x101                 /* the mutex's key */

static volatile int counter;
static volatile int done;           /* threads that finished the round */
static int mtx;
static int use_mutex;

static void bump(void)
{
    for (int i = 0; i < N; i++) {
        if (use_mutex)
            mutex_lock(mtx);
        int tmp = counter;
        sys_sleep(1);               /* a switch, right in the middle */
        counter = tmp + 1;
        if (use_mutex)
            mutex_unlock(mtx);
    }
    done++;
}

static void worker(void *arg)
{
    (void)arg;
    bump();
    sys_exit(0);
}

static int round_run(int with_mutex)
{
    counter = 0;
    done = 0;
    use_mutex = with_mutex;
    sys_thread(worker, 0);
    sys_thread(worker, 0);
    while (done < 2)
        sys_sleep(10);
    return counter;
}

void _start(void)
{
    mtx = mutex_create(MKEY);
    if (mtx < 0) {
        print("[mutex] no free semaphore\n");
        sys_exit(1);
    }

    int without = round_run(0);
    int with = round_run(1);

    print("[mutex] unprotected=");
    print_dec((uint32_t)without);
    print(" with the mutex=");
    print_dec((uint32_t)with);
    print(" (expected ");
    print_dec(2 * N);
    print(")\n");

    sys_exit(with == 2 * N && without < 2 * N ? 0 : 1);
}
