/* sigtest - signals: a handler of one's own, ignoring, default
 * reactions and Ctrl+C
 *
 * A signal is always delivered on the return to ring 3. When a process
 * has a handler of its own, the kernel puts a frame of the interrupted
 * state onto its stack and jumps into the handler; that ends with an
 * ordinary 'ret' to the trampoline, which asks the kernel to restore
 * that state.
 *
 * That is why the sharpest check here is the result of an interrupted
 * syscall: the signal arrives exactly at the moment the kernel has
 * already put zero into eax as the result of kill(). If the frame comes
 * back damaged, sys_kill() will hand back rubbish instead of zero.
 */

#include "usys.h"

static volatile int32_t hits;
static volatile int32_t last_sig;
static volatile int32_t chld;

static void on_signal(int32_t sig)
{
    hits++;
    last_sig = sig;
}

static void on_chld(int32_t sig)
{
    (void)sig;
    chld++;
}

static void fail(const char *msg)
{
    print("[sigtest] WRONG: ");
    print(msg);
    print("\n");
    sys_exit(1);
}

void _start(void)
{
    /* 1. A handler of one's own and a return to the same place */
    if (sys_signal(SIGINT, on_signal) < 0)
        fail("the handler cannot be set");
    int32_t rc = sys_kill(sys_getpid(), SIGINT);
    if (rc != 0)
        fail("an interrupted syscall did not give its result back");
    if (hits != 1 || last_sig != SIGINT)
        fail("the handler never got SIGINT");
    print("[sigtest] my own handler got SIGINT and came back\n");

    /* 2. Ignoring */
    sys_signal(SIGINT, (sighandler_t)SIG_IGN);
    sys_kill(sys_getpid(), SIGINT);
    sys_sleep(50);
    if (hits != 1)
        fail("an ignored signal entered the handler anyway");
    print("[sigtest] SIG_IGN: the signal was lost, as it should be\n");

    /* 3. The default reaction: the child dies of SIGTERM */
    sys_signal(SIGCHLD, on_chld);
    int32_t kid = sys_fork();
    if (kid == 0) {
        sys_signal(SIGTERM, (sighandler_t)SIG_DFL);
        for (;;)
            sys_sleep(100);          /* it waits for its own end */
    }
    sys_sleep(200);
    sys_kill(kid, SIGTERM);
    int32_t code = 0;
    if (sys_waitany(&code) != kid || code != -SIGTERM)
        fail("SIGTERM did not end the child by default");
    print("[sigtest] SIGTERM ends a process by default (code -15)\n");
    if (chld < 1)
        fail("the parent never got SIGCHLD after the child died");
    print("[sigtest] the parent got SIGCHLD\n");

    /* 4. SIGKILL cannot be caught */
    int32_t tough = sys_fork();
    if (tough == 0) {
        sys_signal(SIGKILL, on_signal);   /* it should refuse */
        for (;;)
            sys_sleep(100);
    }
    sys_sleep(200);
    sys_kill(tough, SIGKILL);
    if (sys_waitany(&code) != tough || code != -9)
        fail("SIGKILL could be caught, or did not kill");
    print("[sigtest] SIGKILL cannot be caught (code -9)\n");

    /* 5. Ctrl+C from the keyboard: the driver turns it into a SIGINT
       for the process holding the console, and it interrupts the wait
       for a key */
    hits = 0;
    sys_signal(SIGINT, on_signal);
    print("[sigtest] waiting for Ctrl+C\n");
    char c;
    while (hits == 0) {
        if (sys_read(STDIN, &c, 1) < 0)
            continue;                /* a read interrupted by a signal */
    }
    if (last_sig != SIGINT)
        fail("Ctrl+C arrived as something other than SIGINT");
    print("[sigtest] Ctrl+C arrived as SIGINT\n");
    sys_exit(0);
}
