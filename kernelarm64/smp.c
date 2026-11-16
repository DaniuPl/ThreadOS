/* Waking the other cores - PSCI
 *
 * On a PC the first core wakes the others itself, with INIT and SIPI
 * messages through the local APIC. On ARM that is the firmware's job,
 * behind a standard interface: PSCI. A call is a function number in x0
 * and arguments in x1-x3, made with 'hvc' or 'smc' depending on where
 * the firmware lives - the device tree's /psci says which. CPU_ON takes
 * the core's MPIDR, the physical address it should start at and one
 * value it finds in x0 when it does; here, its index.
 *
 * The new core starts with its MMU off, in secondary_entry (boot.S),
 * on a stack this core took from the heap for it. It cannot take part
 * in anything until its MMU is on - with it off, all memory is Device
 * memory and the exclusive accesses every lock is made of do not work
 * - so it says nothing until secondary_main has switched it on.
 */

#include <stdint.h>
#include "arm64.h"
#include "fdt.h"
#include "thread.h"
#include "uart.h"
#include "../kernel/include/heap.h"

#define PSCI_CPU_ON  0xC4000003u         /* the 64-bit calling convention */
#define STACK_SIZE   (16 * 1024)

extern char secondary_entry[];
/* Read by secondary_entry before its MMU is on */
uint64_t secondary_stacks[MAX_CORES];

static volatile int online = 1;

static int64_t psci_call(int smc, uint64_t fn, uint64_t a1, uint64_t a2,
                         uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    if (smc)
        __asm__ volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3)
                         : "memory");
    else
        __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3)
                         : "memory");
    return (int64_t)x0;
}

void secondary_main(uint64_t index)
{
    exceptions_init();
    mmu_switch_on();
    sched_init_cpu((unsigned)index);
    gic_init_cpu();
    timer_start(HZ);
    __atomic_add_fetch(&online, 1, __ATOMIC_RELEASE);
    /* From here on this boot context is the core's idle thread: the
       timer comes, and whatever is ready gets the core */
    irq_enable();
    for (;;)
        __asm__ volatile("wfi");
}

int smp_start(const struct fdt_info *fi)
{
    if (!fi->psci) {
        printk("Cores online: 1 (no PSCI in the device tree)\n");
        return 1;
    }
    int n = fi->cpus < MAX_CORES ? fi->cpus : MAX_CORES;
    for (int i = 1; i < n; i++) {
        void *stack = kmalloc(STACK_SIZE);
        if (!stack)
            break;
        secondary_stacks[i] =
            ((uintptr_t)stack + STACK_SIZE) & ~(uintptr_t)15;
        /* Out of this core's cache and into memory, where a core with
           its caches still off reads it */
        __asm__ volatile("dc civac, %0; dsb sy"
                         : : "r"(&secondary_stacks[i]) : "memory");

        int want = online + 1;
        int64_t r = psci_call(fi->psci == 2, PSCI_CPU_ON, fi->cpu_id[i],
                              (uint64_t)(uintptr_t)secondary_entry,
                              (uint64_t)i);
        if (r != 0) {
            printk("Core %d (MPIDR 0x%lx) did not start: PSCI said %ld\n",
                   i, fi->cpu_id[i], r);
            continue;
        }
        uint64_t give_up = counter_now() + timer_freq();
        while (__atomic_load_n(&online, __ATOMIC_ACQUIRE) < want &&
               counter_now() < give_up)
            ;
        if (online < want)
            printk("Core %d (MPIDR 0x%lx) was started but never came up\n",
                   i, fi->cpu_id[i]);
    }
    printk("Cores online: %d (started through PSCI, %s)\n", online,
           fi->psci == 2 ? "smc" : "hvc");
    return online;
}
