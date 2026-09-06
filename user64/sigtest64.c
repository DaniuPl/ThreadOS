/* sigtest64 - a program that catches Ctrl+C
 *
 * It sets a handler of its own and waits. After the signal the handler
 * prints that it got it and returns - and the program is to keep
 * running exactly where it was interrupted. The latter matters more
 * than the former: a handler does not end the program, it steps into it
 * mid-stride and steps out again.
 */

#include "usys64.h"

static volatile int dostalem;
static volatile uint64_t spins;

static void on_signal(int sig)
{
    (void)sig;
    print("[sigtest64] the handler: I got a signal\n");
    dostalem = 1;
}

void _start(void)
{

    sys_signal(SIGINT, on_signal);
    print("[sigtest64] waiting for Ctrl+C\n");

    /* An endless loop: if the signal did not work, the program would
       stay here forever. The counter shows that the loop really ran. */
    while (!dostalem) {
        spins++;
        sys_sleep(10);
    }

    print("[sigtest64] I came back where I was interrupted and I am done\n");
    sys_exit(0);
}
