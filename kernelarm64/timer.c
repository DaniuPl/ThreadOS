/* The generic timer - the PIT of this machine, built into every core
 *
 * A counter (CNTVCT_EL0) runs at a fixed rate the firmware states in
 * CNTFRQ_EL0, and the same on every core. Each core has a comparator:
 * when the counter passes CNTV_CVAL_EL0, the virtual timer raises its
 * private interrupt, ID 27 (PPI 11, the number Arm's own base system
 * requirements give it). Every core runs its own, which is what lets
 * every core be preempted.
 *
 * The next deadline is the last one plus the interval, not "now" plus
 * the interval: a tick handled late does not push every later tick
 * back, so a hundred ticks are a second by the counter however slow
 * the machine was to answer them - and an emulator is slow.
 */

#include <stdint.h>
#include "arm64.h"

#define VTIMER_INTID 27

static uint64_t interval;

uint64_t counter_now(void)
{
    __asm__ volatile("isb");     /* else the read may run ahead of the code */
    return read_sysreg(cntvct_el0);
}

uint64_t timer_freq(void)
{
    return read_sysreg(cntfrq_el0);
}

static void tick(void)
{
    struct cpu *c = cpu_this();
    c->ticks++;
    c->timer_next += interval;
    write_sysreg(cntv_cval_el0, c->timer_next);
    c->need_resched = 1;
}

void timer_start(unsigned hz)
{
    struct cpu *c = cpu_this();
    interval = timer_freq() / hz;
    c->timer_next = counter_now() + interval;
    write_sysreg(cntv_cval_el0, c->timer_next);
    write_sysreg(cntv_ctl_el0, 1);       /* enabled, not masked */
    /* A program may read the counter (EL0VCTEN) - the C library's
       start-up seeds from it - but not set the timers */
    write_sysreg(cntkctl_el1, (read_sysreg(cntkctl_el1) & ~0x3FFUL) | 2);
    gic_enable(VTIMER_INTID, tick);
}

/* The first core's ticks: the one clock everything is measured by */
uint64_t timer_ticks(void)
{
    return cpus[0].ticks;
}
