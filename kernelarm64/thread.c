/* Threads and the scheduler of the ARM64 kernel
 *
 * One list of threads and one lock for every core: a core that needs
 * something to run takes the lock and walks the list round-robin from
 * the thread after its current one, for the first that is ready. Each
 * core also has an idle thread, outside the list, for when nothing is.
 *
 * The lock is held ACROSS the switch, and let go by whatever runs
 * next on that core (finish_switch). A thread is marked ready the
 * moment its core decides to leave it, but until its registers are on
 * its stack, another core that picked it up would load a stack pointer
 * that is not yet true. Holding the lock until the switch is done is
 * what makes "ready" mean "on no core": no other core can look before
 * then. (A separate "on which core" mark was tried, and a test with it
 * removed still passed: this already covers it.)
 *
 * A thread that exits cannot free the stack it is standing on; it is
 * marked EXITING, and the next thread on that core, standing on its
 * own stack, marks it DEAD. thread_join gives the memory back.
 *
 * Preemption: the timer sets need_resched on its core, and on the way
 * out of the interrupt sched_preempt switches - on the interrupted
 * thread's own stack, with the interrupt's frame still on it. When the
 * thread is picked again, it returns through that frame as if nothing
 * had happened.
 */

#include <stdint.h>
#include "arm64.h"
#include "thread.h"
#include "../kernel/include/heap.h"
#include "../kernel/include/spinlock.h"

#define STACK_SIZE (16 * 1024)

struct cpu cpus[MAX_CORES];

void switch_to(uint64_t *old_sp, uint64_t new_sp);
void fp_save(uint64_t *state);
void fp_restore(const uint64_t *state);
uint64_t mmu_kernel_ttbr0(void);

/* What a program's thread carries besides its registers on the stack
   (thread.h). Saved and loaded here, before the switch: the kernel
   uses no FP/SIMD register and runs in any address space - the
   kernel's half of every table is the same - so nothing it does in
   between can see the difference, and only the way back to the program
   needs them. A thread of the kernel's own runs in the kernel's tables,
   so that a program's can be freed once its thread is gone. */
static void switch_program_state(struct thread *old, struct thread *next)
{
    if (old->fpstate) {
        fp_save(old->fpstate);
        old->tpidr_el0 = read_sysreg(tpidr_el0);
    }
    if (next->fpstate) {
        fp_restore(next->fpstate);
        write_sysreg(tpidr_el0, next->tpidr_el0);
    }
    uint64_t want = next->ttbr0 ? next->ttbr0 : mmu_kernel_ttbr0();
    if (read_sysreg(ttbr0_el1) != want) {
        write_sysreg(ttbr0_el1, want);
        /* No address-space numbers yet: what this core cached of the
           old program's pages goes */
        __asm__ volatile("isb; tlbi vmalle1; dsb nsh; isb" ::: "memory");
    }
}

static struct spinlock rq = SPINLOCK_FREE;
static struct thread *threads;
static struct thread main_thread;
static struct thread boot_idle[MAX_CORES];
static int next_id = 1;
static volatile int running;

/* With interrupts off for the two loads: a preemption between finding
   this core and reading its current thread could move the thread to
   another core, and the answer would be that core's thread */
struct thread *thread_current(void)
{
    irqflags_t f = irq_save();
    struct thread *t = cpu_this()->current;
    irq_restore(f);
    return t;
}

/* Runs in the thread switched to, on the core that switched */
static void finish_switch(void)
{
    struct cpu *c = cpu_this();
    struct thread *prev = c->prev;
    if (prev && prev->state == T_EXITING)
        prev->state = T_DEAD;
    c->prev = 0;
    spin_unlock(&rq);
}

static struct thread *pick(struct thread *old)
{
    uint64_t now = counter_now();
    for (struct thread *t = threads; t; t = t->next)
        if (t->state == T_SLEEPING && now >= t->wake_at)
            t->state = T_READY;

    struct thread *start = old->next ? old->next : threads;
    struct thread *t = start;
    if (!t)
        return 0;
    do {
        if (t->state == T_READY)
            return t;
        t = t->next ? t->next : threads;
    } while (t != start);
    return 0;
}

/* With interrupts off. The current thread becomes 'state' and another
   one runs, if there is one that may - unless it was to sleep and
   'unless' says it need not (thread_wait), which is looked at here,
   under the lock that thread_wake takes too. */
static void reschedule_unless(const volatile int *unless, int signals,
                              int state, uint64_t wake_at)
{
    struct cpu *c = cpu_this();
    spin_lock(&rq);
    struct thread *old = c->current;
    if (state == T_SLEEPING && ((unless && *unless) || old->interrupted ||
                                (signals && thread_signalled(old)))) {
        spin_unlock(&rq);
        return;
    }
    old->wake_at = wake_at;
    old->state = state;

    struct thread *next = pick(old);
    if (!next)
        next = state == T_READY ? old : c->idle;
    if (next == old) {
        old->state = T_RUNNING;
        spin_unlock(&rq);
        return;
    }
    next->state = T_RUNNING;
    next->cores |= 1u << c->id;
    c->prev = old;
    c->current = next;
    switch_program_state(old, next);
    switch_to(&old->sp, next->sp);
    finish_switch();
}

static void reschedule(int state, uint64_t wake_at)
{
    reschedule_unless(0, 0, state, wake_at);
}

/* Where a new thread's first switch returns to */
static void thread_entry(void)
{
    finish_switch();
    struct thread *t = cpu_this()->current;   /* before it can move */
    irq_enable();
    t->fn(t->arg);
    thread_exit();
}

static void idle_loop(void *arg)
{
    (void)arg;
    for (;;)
        __asm__ volatile("wfi");
}

/* A thread with a stack of its own, not yet in the list. Its stack
   starts as if switch_to had saved it: twelve registers, of which only
   x30 matters - where 'ret' goes the first time. */
static struct thread *new_thread(const char *name, void (*fn)(void *),
                                 void *arg)
{
    struct thread *t = kmalloc(sizeof(*t));
    void *stack = kmalloc(STACK_SIZE);
    if (!t || !stack) {
        kfree(t);
        kfree(stack);
        return 0;
    }
    uintptr_t top = ((uintptr_t)stack + STACK_SIZE) & ~(uintptr_t)15;
    uint64_t *frame = (uint64_t *)top - 12;
    for (int i = 0; i < 12; i++)
        frame[i] = 0;
    frame[11] = (uint64_t)(uintptr_t)thread_entry;

    t->sp = (uint64_t)(uintptr_t)frame;
    t->stack = stack;
    t->fn = fn;
    t->arg = arg;
    t->name = name;
    t->id = 0;
    t->state = T_READY;
    t->wake_at = 0;
    t->cores = 0;
    t->next = 0;
    t->proc = 0;
    t->ttbr0 = 0;
    t->tpidr_el0 = 0;
    t->fpstate = 0;
    t->tid = 0;
    t->clear_tid = 0;
    t->ended = 0;
    t->interrupted = 0;
    t->woken = 0;
    t->sig_pending = 0;
    t->sig_blocked = 0;
    t->sig_saved = 0;
    t->sig_suspended = 0;
    t->sig_bythread = 0;
    return t;
}

static struct thread *add_thread(struct thread *t)
{
    irqflags_t f = spin_lock_irq(&rq);
    t->id = next_id++;
    struct thread **pp = &threads;
    while (*pp)
        pp = &(*pp)->next;
    *pp = t;
    spin_unlock_irq(&rq, f);
    return t;
}

struct thread *thread_create(const char *name, void (*fn)(void *), void *arg)
{
    struct thread *t = new_thread(name, fn, arg);
    return t ? add_thread(t) : 0;
}

/* Everything a program's thread carries is in place before it can be
   picked: the first switch to it loads its tables. It is held - asleep
   with no time to wake - until thread_wake: what its program has to
   know of it (its number, written where the C library looks) is
   written before it can run on another core. */
struct thread *thread_create_program(const char *name, void (*fn)(void *),
                                     void *arg, struct uproc *p,
                                     uint64_t ttbr0, uint64_t tpidr_el0)
{
    struct thread *t = new_thread(name, fn, arg);
    uint64_t *fp = kmalloc(FPSTATE_WORDS * sizeof(uint64_t));
    if (!t || !fp) {
        if (t) {
            kfree(t->stack);
            kfree(t);
        }
        kfree(fp);
        return 0;
    }
    for (int i = 0; i < FPSTATE_WORDS; i++)
        fp[i] = 0;
    t->proc = p;
    t->ttbr0 = ttbr0;
    t->tpidr_el0 = tpidr_el0;
    t->fpstate = fp;
    t->state = T_SLEEPING;
    t->wake_at = ~0UL;
    return add_thread(t);
}

void thread_yield(void)
{
    irqflags_t f = irq_save();
    reschedule(T_READY, 0);
    irq_restore(f);
}

void thread_sleep_ms(unsigned ms)
{
    irqflags_t f = irq_save();
    reschedule(T_SLEEPING, counter_now() + (uint64_t)ms * timer_freq() / 1000);
    irq_restore(f);
}

void thread_exit(void)
{
    irq_disable();
    reschedule(T_EXITING, 0);
    arm64_halt();                    /* never comes back here */
}

int thread_try_join(struct thread *t)
{
    irqflags_t f = spin_lock_irq(&rq);
    if (t->state != T_DEAD) {
        spin_unlock_irq(&rq, f);
        return 0;
    }
    struct thread **pp = &threads;
    while (*pp && *pp != t)
        pp = &(*pp)->next;
    if (*pp)
        *pp = t->next;
    spin_unlock_irq(&rq, f);
    kfree(t->stack);
    kfree(t->fpstate);
    kfree(t);
    return 1;
}

void thread_join(struct thread *t)
{
    while (!thread_try_join(t))
        thread_sleep_ms(1);
}

void thread_wait(const volatile int *flag, uint64_t until)
{
    irqflags_t f = irq_save();
    reschedule_unless(flag, 1, T_SLEEPING, until);
    irq_restore(f);
}

void thread_wake(struct thread *t)
{
    irqflags_t f = spin_lock_irq(&rq);
    if (t->state == T_SLEEPING)
        t->state = T_READY;
    spin_unlock_irq(&rq, f);
}

uint64_t thread_stack_top(struct thread *t)
{
    return ((uint64_t)(uintptr_t)t->stack + STACK_SIZE) & ~15UL;
}

void thread_interrupt(struct thread *t)
{
    t->interrupted = 1;
    thread_wake(t);
}

void sched_preempt(void)
{
    struct cpu *c = cpu_this();
    if (!running || !c->need_resched)
        return;
    c->need_resched = 0;
    reschedule(T_READY, 0);
}

void sched_init(void)
{
    struct cpu *c = &cpus[0];
    c->id = 0;
    write_sysreg(tpidr_el1, (uint64_t)(uintptr_t)c);

    main_thread.name = "main";
    main_thread.state = T_RUNNING;
    main_thread.cores = 1;
    threads = &main_thread;
    c->current = &main_thread;
    /* The first core's boot stack is main's, so its idle thread needs
       a stack of its own */
    c->idle = new_thread("idle", idle_loop, 0);
    running = 1;
}

void sched_init_cpu(unsigned index)
{
    struct cpu *c = &cpus[index];
    c->id = index;
    write_sysreg(tpidr_el1, (uint64_t)(uintptr_t)c);
    struct thread *t = &boot_idle[index];
    t->name = "idle";
    t->state = T_RUNNING;
    t->cores = 1u << index;
    c->idle = c->current = t;
}
