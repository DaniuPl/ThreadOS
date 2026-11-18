/* spin - a program that never gives the core up
 *
 * Started first on a one-core machine, it would keep the core for a
 * second and a half unless the timer takes it: the test's other
 * program, started after it, has to finish first.
 */
#include "usys.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int64_t until = sys_ticks() + 150;
    volatile uint64_t n = 0;
    while (sys_ticks() < until)
        n++;
    print("spin: done\n");
    return 0;
}
