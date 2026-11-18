#ifndef THREADOS_ARM64_H
#define THREADOS_ARM64_H

#include <stdint.h>

/* System registers are read and written by name, and the name is part
   of the instruction - so these are macros, not functions */
#define read_sysreg(r) ({                                   \
    uint64_t __v;                                           \
    __asm__ volatile("mrs %0, " #r : "=r"(__v));            \
    __v;                                                    \
})
#define write_sysreg(r, v)                                  \
    __asm__ volatile("msr " #r ", %0" : : "r"((uint64_t)(v)))

static inline void irq_enable(void)
{
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

static inline void irq_disable(void)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

/* Stops this core for good: nothing is unmasked, so nothing wakes it */
static inline __attribute__((noreturn)) void arm64_halt(void)
{
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    for (;;)
        __asm__ volatile("wfi");
}

#define MAX_CORES 8
#define HZ 100

/* What each core keeps of its own. TPIDR_EL1 - a register the
   processor gives the kernel for exactly this - points at its entry, so
   finding "this core" is one instruction and needs no lock. */
struct thread;
struct cpu {
    unsigned id;                 /* its index in the device tree */
    struct thread *current;      /* what it is running */
    struct thread *idle;         /* what it runs when nothing is ready */
    struct thread *prev;         /* what it just switched away from */
    volatile int need_resched;   /* set by the timer, acted on after it */
    uint64_t timer_next;         /* its timer's next deadline */
    volatile uint64_t ticks;
};
extern struct cpu cpus[MAX_CORES];

static inline struct cpu *cpu_this(void)
{
    return (struct cpu *)read_sysreg(tpidr_el1);
}

/* exceptions.c - the vector table in vectors.S calls into here */
struct trap_frame {
    uint64_t x[31];
    uint64_t elr, spsr, sp_el0;
};
void exceptions_init(void);

/* mmu.c: the tables, built once; then each core switches its MMU on */
int mmu_build(uint64_t ram_base, uint64_t ram_size);
void mmu_switch_on(void);

/* gic.c */
int gic_init(int version, uint64_t dist, uint64_t cpu);
void gic_init_cpu(void);
void gic_enable(unsigned intid, void (*handler)(void));
void gic_irq(void);

/* timer.c - per core */
void timer_start(unsigned hz);
uint64_t timer_ticks(void);
uint64_t timer_freq(void);
uint64_t counter_now(void);

/* pmm.c - the page allocator behind kernel/include/pmm.h */
void pmm_init_range(uint64_t ram_base, uint64_t ram_size, uint64_t first_free);

/* smp.c */
struct fdt_info;
int smp_start(const struct fdt_info *fi);

#endif
