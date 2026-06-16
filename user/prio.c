/* prio - demonstracja schedulera z priorytetami
 *
 * Watek glowny ustawia sobie wysoki priorytet (by moc zatrzymac test),
 * tworzy dwa watki liczace w petli: jeden normalny, drugi niski. Przez
 * okno czasu watek normalny wywlaszcza niski, wiec naliczy znacznie
 * wiecej. Po czasie glowny ustawia 'stop' i wypisuje oba liczniki.
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
    sys_setprio(2);              /* niski priorytet */
    while (!stop)
        cnt_low++;
    sys_exit(0);
}

void _start(void)
{
    sys_setprio(0);             /* glowny watek: wysoki priorytet */
    int n = sys_thread(worker_normal, 0);
    int l = sys_thread(worker_low, 0);
    if (n < 0 || l < 0) {
        print("[prio] blad tworzenia watkow\n");
        sys_exit(1);
    }

    sys_sleep(800);             /* okno pomiaru - glowny spi */
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
