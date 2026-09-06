/* cores - which processor core am I really running on
 *
 * The program forks and counts in two processes at once. Each of them
 * checks for itself where it is being run - the core number cannot be
 * read from ring 3, because the local APIC lies outside the program's
 * address space, so it asks the kernel about it.
 *
 * The fork is not decoration. A task that runs alone stays where it
 * started: the scheduler has no reason to move it when nobody else is
 * waiting. Only TWO counting at once have to land on two cores - and
 * then both finish in the time of one, not of two.
 *
 * Each process prints three things and each of them is for something:
 * the mask of cores that ran it, the tick count (that is, whether it
 * really went in parallel) and the checksum (that is, whether it
 * computed what it was supposed to).
 *
 * Usage: run cores   - both processes on the bootstrap core
 *        runw cores  - they are allowed to run on any core
 */

#include "uapi.h"

#define STEPS  60000000u

static uint32_t compute(uint32_t *mask_out)
{
    uint32_t stan = 12345u, sum = 0, mask = 0;
    for (uint32_t i = 0; i < STEPS; i++) {
        stan = stan * 1103515245u + 12345u;
        sum += stan >> 16;
        /* Asking about the core is a system call, and those run under the
           big kernel lock - so we ask rarely, in order to measure the
           counting rather than the wait for the lock */
        if ((i & 0x1FFFFF) == 0)
            mask |= 1u << (uint32_t)sys_core();
    }
    *mask_out = mask;
    return sum;
}

static void report(const char *who, uint32_t mask, uint32_t ticks,
                    uint32_t sum)
{
    print("[cores] ");
    print(who);
    print(": cores ");
    print_dec(mask);
    print(", ticks ");
    print_dec(ticks);
    print(", sum ");
    print_dec(sum);
    print("\n");
}

void _start(void)
{
    int32_t pid = sys_fork();
    if (pid < 0) {
        print("[cores] WRONG: the fork did not work\n");
        sys_exit(1);
    }

    uint32_t start = sys_getticks();
    uint32_t mask = 0;
    uint32_t sum = compute(&mask);
    uint32_t ticks = sys_getticks() - start;

    if (pid == 0) {
        report("child", mask, ticks, sum);
        sys_exit(0);
    }

    int code = 0;
    sys_wait_child(&code);
    report("parent ", mask, ticks, sum);
    print("[cores] done\n");
    sys_exit(0);
}
