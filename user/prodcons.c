/* prodcons - a producer and a consumer on counting semaphores
 *
 * The classic bounded buffer: the producer puts the numbers 1..ITEMS in,
 * the consumer takes them out and adds them up. The buffer has only
 * SLOTS places, so each side has to wait for the other:
 *
 *   empty - how many places are free (starts at SLOTS)
 *   full  - how many numbers are waiting (starts at 0)
 *   mutex - one side at a time inside the buffer
 *
 * The right sum, and the numbers taken out in the order they went in,
 * say that nothing was lost and nothing taken twice - with twice as
 * many items as places, the producer HAS to have waited for the
 * consumer at least once, and the semaphore is what made it wait.
 *
 * Built for both kernels from this one source (uapi.h picks the side).
 */

#include "uapi.h"
#include "usem.h"

#define SLOTS   4
#define ITEMS   8
#define K_EMPTY 0x201
#define K_FULL  0x202
#define K_MTX   0x203

static volatile int buf[SLOTS];
static volatile int head, tail;
static volatile int sum;
static volatile int consumed;
static volatile int in_order = 1;
static volatile int waited;          /* times the buffer filled up */

static int s_empty, s_full, mtx;

static void producer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= ITEMS; i++) {
        sys_sem_wait(s_empty);       /* a free place */
        mutex_lock(mtx);
        buf[head % SLOTS] = i;
        head++;
        if (head - tail == SLOTS)
            waited++;                /* full now - the next one will wait */
        mutex_unlock(mtx);
        sys_sem_post(s_full);        /* a number is waiting */
    }
    sys_exit(0);
}

static void consumer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= ITEMS; i++) {
        sys_sem_wait(s_full);        /* something to take */
        mutex_lock(mtx);
        int v = buf[tail % SLOTS];
        tail++;
        mutex_unlock(mtx);
        sys_sem_post(s_empty);       /* the place is free again */
        if (v != i)
            in_order = 0;
        sum += v;
        consumed++;
        sys_sleep(5);                /* a slow consumer, so the buffer fills */
    }
    sys_exit(0);
}

void _start(void)
{
    s_empty = sys_sem_get(K_EMPTY, SLOTS);
    s_full = sys_sem_get(K_FULL, 0);
    mtx = mutex_create(K_MTX);
    if (s_empty < 0 || s_full < 0 || mtx < 0) {
        print("[prodcons] no free semaphores\n");
        sys_exit(1);
    }

    sys_thread(producer, 0);
    sys_thread(consumer, 0);
    while (consumed < ITEMS)
        sys_sleep(10);

    int want = ITEMS * (ITEMS + 1) / 2;
    print("[prodcons] taken=");
    print_dec((uint32_t)consumed);
    print(" sum=");
    print_dec((uint32_t)sum);
    print(" (expected ");
    print_dec((uint32_t)want);
    print(") in order=");
    print(in_order ? "yes" : "NO");
    print(" buffer full=");
    print_dec((uint32_t)waited);
    print("\n");

    sys_exit(sum == want && in_order ? 0 : 1);
}
