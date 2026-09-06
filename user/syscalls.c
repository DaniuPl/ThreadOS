/* syscalls - how many entries into the kernel can be made at once
 *
 * The 'cores' program measured COUNTING in ring 3: there two processes
 * on two cores go side by side, because they do not touch the kernel
 * at all. This one measures something else - ENTERING THE KERNEL
 * itself. The loop computes almost nothing, it only calls the kernel
 * as often as it can.
 *
 * The difference is made by the big kernel lock. As long as every
 * syscall took it, two processes on two cores waited for each other in
 * turn and 'runw' gave nothing - it even hurt, because on top of the
 * waiting came a fight over the same memory line. A call that reaches
 * only for the calling task's state does not need the lock.
 *
 * We measure sys_getpid, because it is the cleanest entry into the
 * kernel there is: it reads one field of its own task and returns.
 * Along the way it is its own correctness check - each of the million
 * calls is to return THIS process's number. If two cores disagreed
 * about which task they are running, it would show up here and show up
 * at once.
 *
 * Usage: run syscalls   - both processes on the bootstrap core
 *        runw syscalls  - they are allowed to run on any core
 */

#include "uapi.h"

#define CALLS  1000000u

static uint32_t loop(uint32_t *mask_out, uint32_t *matching_out)
{
    int32_t mine = sys_getpid();
    uint32_t mask = 0, matching = 0;

    for (uint32_t i = 0; i < CALLS; i++) {
        if (sys_getpid() == mine)
            matching++;
        /* We ask about the core rarely: that call reaches into the local
           APIC, that is, into a hardware register, and we do not want
           that in a measurement loop */
        if ((i & 0xFFFFu) == 0)
            mask |= 1u << (uint32_t)sys_core();
    }

    *mask_out = mask;
    *matching_out = matching;
    return (uint32_t)mine;
}

static void report(const char *who, uint32_t mask, uint32_t ticks,
                    uint32_t matching)
{
    print("[syscalls] ");
    print(who);
    print(": cores ");
    print_dec(mask);
    print(", ticks ");
    print_dec(ticks);
    print(", calls ");
    print_dec(CALLS);
    print(", matching ");
    print_dec(matching);
    print("\n");
}

void _start(void)
{
    int32_t pid = sys_fork();
    if (pid < 0) {
        print("[syscalls] WRONG: the fork did not work\n");
        sys_exit(1);
    }

    uint32_t start = sys_getticks();
    uint32_t mask = 0, matching = 0;
    loop(&mask, &matching);
    uint32_t ticks = sys_getticks() - start;

    if (pid == 0) {
        report("child", mask, ticks, matching);
        sys_exit(0);
    }

    int code = 0;
    sys_wait_child(&code);
    report("parent ", mask, ticks, matching);
    print("[syscalls] done\n");
    sys_exit(0);
}
