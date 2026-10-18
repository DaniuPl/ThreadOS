/* Waking the remaining processors
 *
 * After power-on a single core is running - the one the firmware chose
 * as the boot processor. The rest wait in place for a signal, and that
 * is a decision of the hardware, not of the system: a processor woken
 * by a SIPI starts in real mode at address (vector << 12), which is
 * exactly how a PC started forty years ago.
 *
 * How many there are is stated by the MADT table in ACPI. They are
 * woken through the local interrupt controller (APIC) by a pair of
 * signals: INIT puts the processor into its initial state, SIPI tells
 * it which address to start from. The manufacturer recommends sending
 * SIPI twice - the second is harmless when the first was enough.
 *
 * A woken processor is no longer left with nothing, though. It takes
 * the kernel's shared segment table and shared exception table - until
 * that moment every fault of its own ended in a triple fault and a
 * silent reset of the machine - and then turns on its OWN timer in the
 * local APIC. A PIT interrupt goes through the PIC to one processor,
 * so without that a secondary core would never get any.
 *
 * It still does not run tasks: the scheduler and the process list were
 * written for a single core and quietly count on it. What it does have
 * is everything such a scheduler will need - an exception table, a
 * timer and a number of its own.
 */

#include <stdint.h>
#include "include/smp.h"
#include "include/acpi.h"
#include "include/heap.h"
#include "include/io.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/spinlock.h"
#include "include/task.h"
#include "include/pit.h"
#include "include/tlb.h"


/* The APIC registers lie high, beyond the memory described at startup -
   gluing them onto the mapping takes different code in each mode. A
   secondary core in long mode also needs the page table address,
   because it builds its own road into long mode. */
#ifdef __x86_64__
#include "../kernel64/paging64.h"
#include "../kernel64/gdt64.h"
#include "../kernel64/task64.h"
#include "../kernel64/syscall64.h"
#include "../kernel64/idt64.h"
#define MAP_PHYS(a, n)   paging64_map_phys((a), (n))
#define IDENTITY_TOP()   paging64_identity_top()
#define LOAD_GDT()    gdt64_load()
#define LOAD_IDT()    idt64_load()
#define LOAD_TSS()    gdt64_load_tss()
#define TSS_SEL_BASE     GDT64_TSS
#define TSS_SEL_STEP     16
#define SYSCALL_ENTRY() syscall64_init()
#include "../kernel64/fpu64.h"
#define ENABLE_SSE()  fpu64_enable_core()
#define SET_PAT()     paging64_pat_init()   /* the same PAT on every core */
#else
#include "include/paging.h"
#define MAP_PHYS(a, n)   paging_map_phys((a), (n))
#define IDENTITY_TOP()   paging_identity_limit()
#include "include/gdt.h"
#include "include/idt.h"
#define LOAD_GDT()    gdt_load()
#define LOAD_IDT()    idt_load()
#define LOAD_TSS()    gdt_load_tss()
#define TSS_SEL_BASE     GDT_TSS
#define TSS_SEL_STEP     8
#define SYSCALL_ENTRY() do { } while (0)   /* i386 enters through the int 0x80 gate */
#define ENABLE_SSE()  do { } while (0)     /* i386 programs do not need XMM */
#define SET_PAT()     do { } while (0)     /* i386 maps the screen uncached */
#endif

#define AP_BASE       0x8000        /* where we copy the trampoline to */
#define AP_STACK      8192          /* one stack per woken core */
#define MAX_CPU       SMP_CORES

/* Local APIC registers (offsets from its base) */
#define APIC_ID       0x020
#define APIC_SVR      0x0F0         /* the spurious interrupt register */
#define APIC_EOI      0x0B0         /* end-of-interrupt acknowledgement */
#define APIC_ICR_LO   0x300
#define APIC_ICR_HI   0x310
#define APIC_LVT_TIM  0x320         /* the timer entry in the APIC interrupt table */
#define APIC_TIM_INIT 0x380         /* the counter's initial value */
#define APIC_TIM_CUR  0x390         /* the counter's current value */
#define APIC_TIM_DIV  0x3E0         /* the timer divisor */

#define LVT_PERIODIC  (1u << 17)    /* the counter restarts by itself */
#define LVT_MASKED (1u << 16)
#define SMP_VEC_TIMER 48
#define SMP_VEC_WAKE 49
#define SMP_VEC_TLB   50    /* a changed mapping - see kernel/tlb.c */
#define ICR_ALL_BUT_SELF 0x000C0000u

#define ICR_INIT      0x00000500
#define ICR_STARTUP   0x00000600
#define ICR_ASSERT    0x00004000
#define ICR_PENDING   0x00001000    /* the signal is still on its way */

/* An MADT entry header: every one starts the same way */
struct madt_wpis {
    uint8_t typ;
    uint8_t len;
} __attribute__((packed));

struct madt_lapic {
    struct madt_wpis header;
    uint8_t acpi_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed));

extern uint8_t ap_trampoline[], ap_trampoline_end[];
extern volatile uint32_t ap_counter;
extern volatile uint32_t ap_stack;
extern volatile uint32_t ap_number;
extern volatile uint32_t ap_cr3;

static volatile uint8_t *apic;
static int cpus, started;
static uint8_t apic_id[MAX_CPU];
static uint32_t timer_step;              /* how many APIC ticks in 10 ms */
static volatile uint32_t ticks_of_core[MAX_CPU];

/* --- The shared work queue -------------------------------------------
 *
 * The entire state shared between cores is these few fields. The
 * number of the next unit is taken under a lock, but computed without
 * it - otherwise the cores would wait for one another instead of
 * working.
 */
static struct spinlock lock = SPINLOCK_FREE;
static void (*work_fn)(uint32_t, void *);
static void *work_arg;
static volatile uint32_t work_count;      /* how many units in the order */
static volatile uint32_t work_next; /* the first one not yet taken */
static volatile uint32_t work_done;
static volatile uint32_t share[MAX_CPU];

static void apic_wr(uint32_t reg, uint32_t v)
{
    *(volatile uint32_t *)(apic + reg) = v;
}

static uint32_t apic_rd(uint32_t reg)
{
    return *(volatile uint32_t *)(apic + reg);
}

/* --- Which core is running right now ----------------------------------
 *
 * The processor answers itself: the number in its local APIC is fixed
 * by the hardware and each one reads its own. We translate it into an
 * ordinal in our table, because the hardware numbers need not be
 * consecutive.
 *
 * Before the APIC is started (and on a machine that has none) there is
 * one core and its number is zero.
 */
int smp_core_apic(void)
{
    if (!apic)
        return 0;
    uint8_t id = (uint8_t)(apic_rd(APIC_ID) >> 24);
    for (int i = 0; i < cpus; i++)
        if (apic_id[i] == id)
            return i;
    return 0;
}

int smp_core(void)
{
    uint16_t tr;
    __asm__ volatile("str %0" : "=r"(tr));
    if (tr < TSS_SEL_BASE)
        return 0;                    /* the segment has not been loaded yet */
    int nr = (tr - TSS_SEL_BASE) / TSS_SEL_STEP;
    return (nr >= 0 && nr < SMP_CORES) ? nr : 0;
}

/* --- A core's timer ---------------------------------------------------
 *
 * The boot core has the PIT and it is the PIT that measures time for
 * the whole system. Secondary cores cannot use it: a PIT interrupt goes
 * through the PIC to one processor, not to all of them. Each of them
 * does have its OWN timer in the local APIC - and that is what gives
 * it interrupts.
 *
 * The frequency of that timer cannot be read; it has to be measured.
 * The boot core does it, because only it has anything to measure with:
 * it starts the APIC counter and watches how far it gets during a
 * known number of PIT ticks.
 */
static void timer_measure(void)
{
    /* Waiting for the PIT has to be bounded. This measurement runs very
       early, and on a machine where the PIT does not tick (or where its
       interrupt does not yet reach anywhere) an unbounded loop would
       hang the whole boot - and in a place that says nothing about
       itself. */
#define DELAY_MAX 20000000u

    apic_wr(APIC_TIM_DIV, 0x3);                  /* divisor 16 */
    apic_wr(APIC_LVT_TIM, LVT_MASKED);      /* counts, but does not report */

    uint32_t start = pit_ticks();
    uint32_t rounds = 0;
    while (pit_ticks() == start && rounds < DELAY_MAX)  /* the tick boundary */
        rounds++;
    apic_wr(APIC_TIM_INIT, 0xFFFFFFFFu);

    start = pit_ticks();
    rounds = 0;
    while (pit_ticks() - start < 10 && rounds < DELAY_MAX)   /* 100 ms */
        rounds++;
    uint32_t remaining = apic_rd(APIC_TIM_CUR);
    apic_wr(APIC_TIM_INIT, 0);                   /* stop */

    uint32_t na_100ms = 0xFFFFFFFFu - remaining;
    timer_step = na_100ms / 10;                  /* per ten milliseconds */
    /* When the measurement failed, giving the timer a value off the
       ceiling is better than none: a core without interrupts can never
       be preempted. */
    if (rounds >= DELAY_MAX || timer_step < 1000 || timer_step > 0x10000000u)
        timer_step = 1000000;
#undef DELAY_MAX
}

/* Turns on the local APIC and its timer on the current core */
static void timer_start(void)
{
    apic_wr(APIC_SVR, apic_rd(APIC_SVR) | 0x100 | 0xFF);
    apic_wr(APIC_TIM_DIV, 0x3);
    apic_wr(APIC_LVT_TIM, SMP_VEC_TIMER | LVT_PERIODIC);
    apic_wr(APIC_TIM_INIT, timer_step);
}

/* Handling an APIC timer tick - called from isr_handler for vector 48.
 *
 * Every core counts its own in a separate word, so no lock is needed
 * here. The acknowledgement goes to THIS core's OWN APIC, not to the
 * PIC: the interrupt did not pass through the controller, so there is
 * nobody there to close it. */
void smp_timer_tick(void)
{
    int r = smp_core();
    ticks_of_core[r]++;
    if (apic)
        apic_wr(APIC_EOI, 0);
    /* The acknowledgement goes BEFORE switching tasks: schedule may not
       come back here for a long while, and the local APIC has nothing
       to wait for. The timer handler on the boot core does the same. */
    if (task_scheduler_ready())
        schedule();
}

/* A wake-up for all the other cores.
 *
 * An idle core stands on a hlt instruction - it is not spinning, so it
 * will not notice by itself that work has appeared. An interrupt pulls
 * it out of there, and the simplest interrupt one can send is one that
 * means nothing: its entire content is the fact that it arrived.
 *
 * The "all but self" shorthand saves working out who is still alive -
 * a core that never woke up will do nothing with it anyway. */
static void wake_others(void)
{
    if (!apic)
        return;
    apic_wr(APIC_ICR_HI, 0);
    apic_wr(APIC_ICR_LO, ICR_ALL_BUT_SELF | SMP_VEC_WAKE);
}

/* The shootdown call: a mapping has changed and the other cores are
 * still holding the old one.
 *
 * The same "all but self" shorthand as above, and for the same reason:
 * working out which cores to name would cost more than the interrupt
 * itself, and a core the request does not concern sees that from the
 * request and goes back to what it was doing. Who is really waited for
 * is decided by tlb.c, not by the address of the signal. */
void smp_send_tlb(void)
{
    if (!apic)
        return;
    apic_wr(APIC_ICR_HI, 0);
    apic_wr(APIC_ICR_LO, ICR_ALL_BUT_SELF | SMP_VEC_TLB);
}

void smp_tlb_eoi(void)
{
    if (apic)
        apic_wr(APIC_EOI, 0);
}

void smp_wakeup_recv(void)
{
    if (apic)
        apic_wr(APIC_EOI, 0);
}

uint32_t smp_ticks(int core)
{
    return (core >= 0 && core < MAX_CPU) ? ticks_of_core[core] : 0;
}

uint32_t smp_share(int core)
{
    return (core >= 0 && core < MAX_CPU) ? share[core] : 0;
}

int smp_workers(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CPU; i++)
        if (share[i])
            n++;
    return n;
}

/* Takes one unit to compute. Returns 0 when there is nothing left.
 *
 * We turn interrupts off while holding the lock on the boot core too:
 * if the timer preempted the thread holding the lock, every other
 * taker would have to wait until its next turn. */
static int take_unit(uint32_t *number)
{
    uint32_t flags = irq_save();
    spin_lock(&lock);
    int have = work_next < work_count;
    if (have)
        *number = work_next++;
    spin_unlock(&lock);
    irq_restore(flags);
    return have;
}

/* The work loop - the same for the boot core and for the secondary ones */
static void run_available(int core)
{
    uint32_t number;
    while (take_unit(&number)) {
        work_fn(number, work_arg);
        share[core]++;
        __atomic_add_fetch(&work_done, 1, __ATOMIC_SEQ_CST);
    }
}

/* Entry of a secondary core from the trampoline. Never returns.
 *
 * This core has neither an exception nor an interrupt table - every
 * fault ends in a triple fault and a reset of the machine. That is why
 * it does one thing here: waits for an order and computes what it was
 * given. */
void smp_ap_main(uint32_t number)
{
    if (number >= MAX_CPU)
        number = MAX_CPU - 1;

    /* The trampoline left this core on its own temporary segment table
       and with no exception table at all - every fault ended in a
       triple fault and a silent reset of the whole machine. Both
       tables are single for the system and nobody changes them once
       built, so pointing at them here is enough. */
    LOAD_GDT();
    LOAD_IDT();
    LOAD_TSS();
    /* Say which space this core is running. The trampoline put the
       kernel's tables into CR3; until the core says so out loud, a
       shootdown has no way of knowing it is there. It goes through the
       same one door as every other switch, so that the published value
       and the processor's cannot drift apart - the cost is one flush of
       a cache that holds nothing yet. */
    tlb_switch(tlb_current_space());
    /* What a page entry's caching bits mean is the core's own MSR, and
       it has to mean on this core what it means on the first one - the
       screen is mapped write-combining there (kernel64/paging64.c) */
    SET_PAT();
    /* The entry registers for the syscall instruction belong to the
       core - one that does not set them will not accept a call at all */
    SYSCALL_ENTRY();
    /* CR0 and CR4 belong to the core. Without this a ring 3 program
       that lands here dies of an ordinary XMM instruction - and on
       x86-64 the compiler uses those without asking. */
    ENABLE_SSE();

    /* Its own timer. A PIT interrupt goes through the PIC to ONE
       processor - this one will never see it. */
    timer_start();
    __asm__ volatile("sti");

    /* The scheduler comes up later than the cores - until then this
       core can only compute what it was ordered to */
    while (!task_scheduler_ready()) {
        if (work_count)
            run_available((int)number);
        else
            __asm__ volatile("hlt");
    }

    /* From now on the core is a full kernel processor: it has an idle
       task of its own and takes from the list whatever it is allowed
       to run. This function does not return. */
    task_core_enter((int)number);

    for (;;) {
        if (work_count) {
            run_available((int)number);
            continue;
        }
        /* An idle core is to STAND STILL, not spin.
         *
         * Spinning costs as much as work - on a virtual machine
         * exactly as much, because every round really has to be
         * executed. Three cores spinning to no purpose took
         * three quarters of the machine away from the boot core, and
         * an ordinary disk write took four times as long.
         *
         * The hlt instruction stops the core until the next interrupt,
         * and those now arrive a hundred times a second from its own
         * timer. An order placed just after the condition was checked
         * will therefore wait one tick at most - and there is nothing
         * to hurry for when the work is measured in hundreds of
         * milliseconds. */
        __asm__ volatile("hlt");
    }
}

void smp_run_work(int core)
{
    if (work_count)
        run_available(core);
}

void smp_work(void (*fn)(uint32_t number, void *data), void *data,
               uint32_t count)
{
    if (!fn || !count)
        return;

    for (int i = 0; i < MAX_CPU; i++)
        share[i] = 0;

    work_fn = fn;
    work_arg = data;
    work_next = 0;
    work_done = 0;
    __atomic_store_n(&work_count, count, __ATOMIC_SEQ_CST);

    /* The order is already visible - now the ones sleeping through it
       have to be woken. The order is not arbitrary: a wake-up sent
       before the write would find the queue still empty. */
    wake_others();

    /* The boot core computes along with the rest: on a single-core
       machine nobody else would compute at all */
    run_available(0);

    /* We wait for those who took a unit just before we did */
    while (__atomic_load_n(&work_done, __ATOMIC_SEQ_CST) < count)
        __asm__ volatile("pause" ::: "memory");

    __atomic_store_n(&work_count, 0, __ATOMIC_SEQ_CST);
}

int smp_cpus(void)    { return cpus ? cpus : 1; }
int smp_started(void) { return started; }

/* A delay counted out on port 0x80 - the PIT may still be busy with
   something else, and what matters here is the order of magnitude, not
   accuracy */
static void delay(uint32_t loops)
{
    for (uint32_t i = 0; i < loops; i++)
        io_wait();
}

static void wait_sent(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(apic_rd(APIC_ICR_LO) & ICR_PENDING))
            return;
        io_wait();
    }
}

/* INIT, and after it two SIPIs - as the processor documentation says */
static void wake_cpu(uint8_t id, uint8_t vector)
{
    apic_wr(APIC_ICR_HI, (uint32_t)id << 24);
    apic_wr(APIC_ICR_LO, ICR_INIT | ICR_ASSERT);
    wait_sent();
    delay(20000);                        /* on the order of ten milliseconds */

    for (int i = 0; i < 2; i++) {
        apic_wr(APIC_ICR_HI, (uint32_t)id << 24);
        apic_wr(APIC_ICR_LO, ICR_STARTUP | vector);
        wait_sent();
        delay(600);                      /* on the order of two hundred microseconds */
    }
}

int smp_init(void)
{
    const uint8_t *madt = acpi_table("APIC");
    if (!madt)
        return 1;

    uint32_t len = (uint32_t)madt[4] | ((uint32_t)madt[5] << 8) |
                       ((uint32_t)madt[6] << 16) | ((uint32_t)madt[7] << 24);
    uint32_t apic_base = (uint32_t)madt[36] | ((uint32_t)madt[37] << 8) |
                         ((uint32_t)madt[38] << 16) |
                         ((uint32_t)madt[39] << 24);
    if (len < 44 || !apic_base)
        return 1;

    /* The APIC registers lie high, beyond the memory described at startup */
    if ((uint64_t)apic_base + 0x1000 > IDENTITY_TOP() &&
        MAP_PHYS(apic_base, 0x1000) < 0)
        return 1;
    apic = (volatile uint8_t *)(uintptr_t)apic_base;

    /* The processor listing begins after the table header */
    for (uint32_t off = 44; off + 2 <= len && cpus < MAX_CPU; ) {
        const struct madt_wpis *w = (const struct madt_wpis *)(madt + off);
        if (!w->len)
            break;
        if (w->typ == 0 && w->len >= sizeof(struct madt_lapic)) {
            const struct madt_lapic *l = (const struct madt_lapic *)w;
            if (l->flags & 1)                 /* the processor is enabled */
                apic_id[cpus++] = l->apic_id;
        }
        off += w->len;
    }
    if (cpus <= 1)
        return cpus ? cpus : 1;

    /* Turn on the boot processor's local APIC: without it there is
       nothing to send the signal with. Spurious vector 0xFF and the
       enable bit. */
    apic_wr(APIC_SVR, apic_rd(APIC_SVR) | 0x100 | 0xFF);

    /* Measure the APIC timer while we still can: the PIT is only here */
    timer_measure();

    uint32_t size = (uint32_t)(ap_trampoline_end - ap_trampoline);
    if (size > 4096)
        return cpus;
    memcpy((void *)(uintptr_t)AP_BASE, ap_trampoline, size);

    /* The counter and the stack pointer lie in the copied block, not in
       the kernel - a processor in real mode will not reach where the
       original sits */
    uint32_t off_counter =
        (uint32_t)((uint8_t *)&ap_counter - ap_trampoline);
    uint32_t off_stack = (uint32_t)((uint8_t *)&ap_stack - ap_trampoline);
    uint32_t off_number = (uint32_t)((uint8_t *)&ap_number - ap_trampoline);
    volatile uint32_t *counter =
        (volatile uint32_t *)(uintptr_t)(AP_BASE + off_counter);
    volatile uintptr_t *stack =
        (volatile uintptr_t *)(uintptr_t)(AP_BASE + off_stack);
    volatile uint32_t *number =
        (volatile uint32_t *)(uintptr_t)(AP_BASE + off_number);
    *counter = 0;

    /* The secondary core takes the same page tables the kernel walks:
       in long mode it will not start at all without them, and in
       protected mode it would start, only user program addresses
       would mean nothing to it. */
    uint32_t off_cr3 = (uint32_t)((uint8_t *)&ap_cr3 - ap_trampoline);
    uintptr_t cr3;
#ifdef __x86_64__
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
#else
    __asm__ volatile("movl %%cr3, %0" : "=r"(cr3));
#endif
    *(volatile uint32_t *)(uintptr_t)(AP_BASE + off_cr3) = (uint32_t)cr3;

    uint8_t mine = (uint8_t)(apic_rd(APIC_ID) >> 24);

    /* We wake them one at a time and wait for each to report in: there
       is one stack, so two processors at once would trample each
       other's frames */
    for (int i = 0; i < cpus; i++) {
        if (apic_id[i] == mine)
            continue;

        void *memory = kmalloc(AP_STACK);
        if (!memory)
            break;
        *stack = (uintptr_t)memory + AP_STACK;
        /* The boot core has number zero, secondary ones get the next -
           each counts its share in a separate word, so they need not
           fight over it */
        *number = (uint32_t)(started + 1);

        uint32_t was = *counter;
        wake_cpu(apic_id[i], AP_BASE >> 12);

        for (int spin = 0; spin < 400000 && *counter == was; spin++)
            io_wait();
        if (*counter > was)
            started++;
    }

    return cpus;
}
