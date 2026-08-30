/* ptree - the process tree: fork, wait for any child, orphans
 *
 * The parent forks three times. Every child sleeps a different while
 * and ends with its own code, so they finish in the reverse order to
 * the one they were created in. The parent collects them through
 * sys_waitany() - it does not have to guess who will finish first - and
 * at the end checks that once there are no children left, waitany says
 * so outright (-1) instead of waiting forever.
 *
 * The second part checks orphans: a child forks a grandchild and dies
 * immediately, leaving it alone. The grandchild is adopted by the
 * grandparent, so its exit code is collected by the same waitany -
 * otherwise it would be lost and the grandchild would have nobody to
 * report to.
 */

#include "usys.h"

#define KIDS 3

void _start(void)
{
    int32_t pid[KIDS];

    for (int i = 0; i < KIDS; i++) {
        int32_t p = sys_fork();
        if (p < 0) {
            print("[ptree] fork sie nie udal\n");
            sys_exit(1);
        }
        if (p == 0) {
            /* The last child sleeps the shortest - it will finish first */
            sys_sleep((uint32_t)(KIDS - i) * 300);
            sys_exit(20 + i);
        }
        pid[i] = p;
    }

    print("[ptree] rodzic ma pid ");
    print_dec((uint32_t)sys_getpid());
    print(", dzieci: ");
    for (int i = 0; i < KIDS; i++) {
        print_dec((uint32_t)pid[i]);
        print(i == KIDS - 1 ? "\n" : " ");
    }

    int got = 0, wrong = 0;
    for (;;) {
        int32_t code = -1;
        int32_t who = sys_waitany(&code);
        if (who < 0)
            break;                      /* there are no children left */

        /* The exit code has to match which child it is */
        int idx = -1;
        for (int i = 0; i < KIDS; i++)
            if (pid[i] == who)
                idx = i;
        if (idx < 0 || code != 20 + idx)
            wrong++;

        print("[ptree] skonczylo dziecko ");
        print_dec((uint32_t)who);
        print(" z kodem ");
        print_dec((uint32_t)code);
        print("\n");
        got++;
    }

    if (got != KIDS || wrong) {
        print("[ptree] zle: zebranych dzieci ");
        print_dec((uint32_t)got);
        print(", niezgodnych kodow ");
        print_dec((uint32_t)wrong);
        print("\n");
        sys_exit(1);
    }
    print("[ptree] zebrane wszystkie dzieci, waitany zglosil koniec\n");

    /* An orphan: the child makes a grandchild and dies at once */
    int32_t mid = sys_fork();
    if (mid < 0) {
        print("[ptree] fork sieroty sie nie udal\n");
        sys_exit(1);
    }
    if (mid == 0) {
        int32_t grand = sys_fork();
        if (grand == 0) {
            sys_sleep(500);         /* it will outlive its own parent */
            sys_exit(99);
        }
        print("[ptree] dziecko robi wnuka ");
        print_dec((uint32_t)grand);
        print(" i konczy sie\n");
        sys_exit(30);
    }

    int seen_child = 0, seen_orphan = 0;
    for (;;) {
        int32_t code = -1;
        int32_t who = sys_waitany(&code);
        if (who < 0)
            break;
        if (code == 30 && who == mid)
            seen_child = 1;
        else if (code == 99)
            seen_orphan = 1;         /* the grandchild was adopted by the grandparent */
        print("[ptree] odebrane od ");
        print_dec((uint32_t)who);
        print(" z kodem ");
        print_dec((uint32_t)code);
        print("\n");
    }
    if (!seen_child || !seen_orphan) {
        print("[ptree] zle: sierota nie trafila pod opieke dziadka\n");
        sys_exit(1);
    }
    print("[ptree] sierota przygarnieta przez dziadka\n");
    sys_exit(0);
}
