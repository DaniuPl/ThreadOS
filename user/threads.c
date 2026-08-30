/* threads - threads within one process (a shared address space)
 *
 * It creates two threads through sys_thread; each sets a different
 * global variable. The main thread waits for both and adds the results
 * up - it sees the threads' writes, because they share the same memory.
 * The expected result: 11 + 22 = 33.
 */

#include "usys.h"

static volatile int va, vb;

static void worker_a(void *arg)
{
    (void)arg;
    va = 11;
    sys_exit(0);
}

static void worker_b(void *arg)
{
    (void)arg;
    vb = 22;
    sys_exit(0);
}

void _start(void)
{
    int32_t t1 = sys_thread(worker_a, 0);
    int32_t t2 = sys_thread(worker_b, 0);
    if (t1 < 0 || t2 < 0) {
        print("[threads] blad tworzenia watkow\n");
        sys_exit(1);
    }
    sys_wait(t1);
    sys_wait(t2);

    print("[threads] watki zapisaly, suma=");
    print_dec((uint32_t)(va + vb));   /* 33 = both threads saw the same memory */
    print("\n");
    sys_exit(0);
}
