#ifndef THREADOS_ARM64_THREAD_H
#define THREADOS_ARM64_THREAD_H

#include <stdint.h>

/* Kernel threads of the ARM64 kernel - one run queue for every core */

enum { T_READY, T_RUNNING, T_SLEEPING, T_EXITING, T_DEAD };

struct uproc;

struct thread {
    uint64_t sp;                 /* first: switch.S saves it here */
    void *stack;                 /* from the heap; 0 for a boot stack */
    void (*fn)(void *);
    void *arg;
    const char *name;
    int id;
    volatile int state;
    uint64_t wake_at;            /* the counter value to sleep until */
    volatile uint32_t cores;     /* every core it has run on, as bits */
    struct thread *next;
    /* A thread of a program (kernelarm64/proc.c) has more to carry
       than a kernel thread: the address space it runs in, the FP/SIMD
       registers the program may use and the kernel does not, and its
       thread pointer (TPIDR_EL0, where a C library keeps its TLS) */
    struct uproc *proc;
    uint64_t ttbr0;              /* its tables, or 0: the kernel's */
    uint64_t tpidr_el0;
    uint64_t *fpstate;           /* q0-q31, then FPCR and FPSR */
    /* and, as a thread of a program, what the program knows it by */
    int tid;                     /* its number (SYS64_GETPID) */
    uint64_t clear_tid;          /* zeroed and woken when it ends */
    volatile int ended;          /* it is past its last instruction */
    volatile int interrupted;    /* to stop waiting: its program ends */
    volatile int woken;          /* thread_wait's flag for IPC waits */
    /* Signals (proc.c), as bits - signal n is bit n-1: those sent to
       it and not yet delivered, those it holds back (sigprocmask), and
       while it waits in sigsuspend, the mask it had before */
    volatile uint64_t sig_pending;
    volatile uint64_t sig_blocked;
    uint64_t sig_saved;
    int sig_suspended;
    /* who sent each (its program's number), and which were sent to
       this thread by name (tkill) rather than to its program (kill) -
       what the handler's siginfo_t says */
    int32_t sig_sender[64];
    uint64_t sig_bythread;
    /* A signal its own instruction raised (SIGILL, SIGSEGV...): which,
       its si_code and the address it is about */
    int sig_fault, sig_fault_code;
    uint64_t sig_fault_addr;
};

/* A signal it does not hold back waits for it: a system call's wait
   ends for that, to be taken up again after the handler (proc.c) */
static inline int thread_signalled(const struct thread *t)
{
    return (t->sig_pending & ~t->sig_blocked) != 0;
}

/* The boot context of the first core becomes the thread "main" */
void sched_init(void);
/* A further core's boot context becomes its idle thread; called on
   that core, with its index */
void sched_init_cpu(unsigned index);

struct thread *thread_create(const char *name, void (*fn)(void *), void *arg);
/* A thread of a program: it runs in that program's tables (ttbr0) and
   has FP/SIMD registers and a thread pointer of its own. It is held
   until thread_wake: the caller finishes setting it up first. */
#define FPSTATE_WORDS 66         /* 32 x 128 bits, FPCR, FPSR */
struct thread *thread_create_program(const char *name, void (*fn)(void *),
                                     void *arg, struct uproc *p,
                                     uint64_t ttbr0, uint64_t tpidr_el0);
void thread_yield(void);
void thread_sleep_ms(unsigned ms);
void thread_exit(void) __attribute__((noreturn));
/* Waits until the thread has finished, then gives its memory back */
void thread_join(struct thread *t);
/* The same if it has finished already (1), and nothing if not (0) */
int thread_try_join(struct thread *t);

/* Sleeps until the counter reaches 'until' (~0: no limit) or the
   thread is woken - and not at all if *flag is set already, the thread
   was interrupted or has a signal to take (thread_signalled). *flag
   and the signals are looked at under the scheduler's lock, as
   thread_wake changes a thread's state: whoever sets them and then
   wakes the thread cannot be missed. thread_sleep_ms, the kernel's own
   pause, is not cut short by a signal. */
void thread_wait(const volatile int *flag, uint64_t until);
/* A sleeping thread is ready again; one that is not, stays as it is */
void thread_wake(struct thread *t);
/* Its waits end now and from now on (its program is ending) */
void thread_interrupt(struct thread *t);
/* The top of its kernel stack, where an exception from EL0 begins */
uint64_t thread_stack_top(struct thread *t);
struct thread *thread_current(void);

/* Called on the way out of an interrupt: the timer may have asked for
   another thread */
void sched_preempt(void);

#endif
