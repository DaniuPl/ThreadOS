/* prio - a demonstration of the scheduler with priorities
 *
 * The main thread gives itself a high priority (so as to be able to
 * stop the test), then creates two threads counting in a loop: one
 * normal, the other low. Over the time window the normal thread
 * preempts the low one, so it will count considerably more. After the
 * time is up the main one sets 'stop' and prints both counters.
 */

#include "usys.h"

static volatile int cnt_normal, cnt_low, stop;

static void worker_normal(void *arg)
{
    (void)arg;
    while (!stop)
        cnt_normal++;
    sys_exit(0);
}

static void worker_low(void *arg)
{
    (void)arg;
    sys_setprio(2);              /* low priority */
    while (!stop)
        cnt_low++;
    sys_exit(0);
}

void _start(void)
{
    sys_setprio(0);             /* the main thread: high priority */
    int n = sys_thread(worker_normal, 0);
    int l = sys_thread(worker_low, 0);
    if (n < 0 || l < 0) {
        print("[prio] blad tworzenia watkow\n");
        sys_exit(1);
    }

    sys_sleep(800);             /* the measurement window - the main one sleeps */
    stop = 1;
    sys_wait(n);
    sys_wait(l);

    print("[prio] normalny=");
    print_dec((uint32_t)cnt_normal);
    print(" niski=");
    print_dec((uint32_t)cnt_low);
    print("\n");
    sys_exit(0);
}
