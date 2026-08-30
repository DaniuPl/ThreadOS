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
    print("[sigtest] ZLE: ");
    print(msg);
    print("\n");
    sys_exit(1);
}

void _start(void)
{
    /* 1. A handler of one's own and a return to the same place */
    if (sys_signal(SIGINT, on_signal) < 0)
        fail("nie da sie ustawic procedury obslugi");
    int32_t rc = sys_kill(sys_getpid(), SIGINT);
    if (rc != 0)
        fail("przerwany syscall nie oddal swojego wyniku");
    if (hits != 1 || last_sig != SIGINT)
        fail("procedura obslugi nie dostala SIGINT");
    print("[sigtest] wlasna procedura obslugi dostala SIGINT i wrocila\n");

    /* 2. Ignoring */
    sys_signal(SIGINT, (sighandler_t)SIG_IGN);
    sys_kill(sys_getpid(), SIGINT);
    sys_sleep(50);
    if (hits != 1)
        fail("zignorowany sygnal jednak wszedl w procedure obslugi");
    print("[sigtest] SIG_IGN: sygnal przepadl, tak jak mial\n");

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
        fail("SIGTERM nie zakonczyl dziecka domyslna reakcja");
    print("[sigtest] SIGTERM domyslnie konczy proces (kod -15)\n");
    if (chld < 1)
        fail("rodzic nie dostal SIGCHLD po smierci dziecka");
    print("[sigtest] rodzic dostal SIGCHLD\n");

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
        fail("SIGKILL dal sie przechwycic albo nie zabil");
    print("[sigtest] SIGKILL nie do przechwycenia (kod -9)\n");

    /* 5. Ctrl+C from the keyboard: the driver turns it into a SIGINT
       for the process holding the console, and it interrupts the wait
       for a key */
    hits = 0;
    sys_signal(SIGINT, on_signal);
    print("[sigtest] czekam na Ctrl+C\n");
    char c;
    while (hits == 0) {
        if (sys_read(STDIN, &c, 1) < 0)
            continue;                /* a read interrupted by a signal */
    }
    if (last_sig != SIGINT)
        fail("Ctrl+C przyszedl jako cos innego niz SIGINT");
    print("[sigtest] Ctrl+C dotarl jako SIGINT\n");
    sys_exit(0);
}
