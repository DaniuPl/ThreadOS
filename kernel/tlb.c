/* TLB shootdown - telling the other cores that a mapping has changed
 *
 * A page table is memory, and memory is shared. A translation worked
 * out FROM that table is not: every core keeps its own, in a cache the
 * kernel cannot reach with any instruction. So changing an entry
 * changes what the tables say and nothing about what another core
 * believes - and that core carries on writing to a frame the kernel
 * may already have freed, copied away from, or handed to somebody
 * else.
 *
 * The only way to reach that cache is to ask its owner: send an
 * interrupt and wait for the answer. The waiting is the whole point.
 * A request that is sent and not awaited says nothing about when it
 * will be honoured, and "when" is exactly what the caller needs to
 * know - it is about to free the frame.
 *
 * One request at a time, for the whole machine. The big kernel lock
 * still serialises nearly everything that changes a mapping, so a
 * second slot would sit unused and would have to be reasoned about
 * anyway. When that lock goes, this is the next thing to grow.
 *
 * --- why the mask cannot be wrong -------------------------------------
 *
 * We ask only the cores that say they are running the space in
 * question, and a core can change its mind while we are asking. Both
 * directions are safe, and for the same reason:
 *
 *   A core LEAVES the space after we counted it. Its answer then
 *   invalidates an address in a space it no longer runs - one wasted
 *   miss - and the switch itself emptied its cache anyway.
 *
 *   A core ENTERS the space after we counted it. To do that it loads
 *   CR3, and loading CR3 throws its whole cache away. It cannot come
 *   in holding something older than the moment it arrived.
 *
 * What must not happen is a core sitting inside the space, holding an
 * old translation, and not being counted - and that is impossible,
 * because it published the space before it could use it.
 */

#include <stdint.h>
#include "include/tlb.h"
#include "include/smp.h"

/* What every core is running. Written by its owner and read by
   everybody, which is why the single door below matters: a value the
   kernel publishes and a value the processor holds must not be able to
   drift apart. */
static volatile uintptr_t core_space[SMP_CORES];

/* The request in flight. Only whoever holds 'busy' writes these, and
   'wait' is written last - the others have to be readable before a
   core can see that it is being asked. */
static volatile uint32_t busy;
static volatile uintptr_t req_space;
static volatile uintptr_t req_addr;    /* 0 = the whole space */
static volatile uint32_t req_kernel;   /* 1 = concerns every core */
static volatile uint32_t req_wait;     /* cores that still owe an answer */

static volatile uint32_t stat_shootdowns;
static volatile uint32_t stat_local;
static volatile uint32_t stat_acks;

static inline void invlpg(uintptr_t a)
{
    __asm__ volatile("invlpg (%0)" : : "r"((void *)a) : "memory");
}

static inline uintptr_t read_cr3(void)
{
    uintptr_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uintptr_t v)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}

static inline void relax(void)
{
    __asm__ volatile("pause" ::: "memory");
}

/* Increment shared by several cores - a plain ++ would lose some */
static inline void bump(volatile uint32_t *p)
{
    __asm__ volatile("lock incl %0" : "+m"(*p) : : "memory");
}

/* Clears our bit in the mask. The answer has to become visible only
   after the work behind it is done, and a locked operation is what
   makes that so. */
static inline void clear_bit(volatile uint32_t *p, uint32_t bit)
{
    __asm__ volatile("lock andl %1, %0" : "+m"(*p) : "r"(~bit) : "memory");
}

static inline int try_lock(volatile uint32_t *p)
{
    uint32_t was = 1;
    __asm__ volatile("xchgl %0, %1" : "+r"(was), "+m"(*p) : : "memory");
    return !was;
}

static inline void unlock(volatile uint32_t *p)
{
    uint32_t zero = 0;
    __asm__ volatile("xchgl %0, %1" : "+r"(zero), "+m"(*p) : : "memory");
}

/* --- the two doors ---------------------------------------------------- */

void tlb_switch(uintptr_t space)
{
    int me = smp_core();
    /* Published BEFORE the switch, never after. A core that is asked
       between the two lines invalidates an address in the space it is
       leaving - wasted work, nothing more. Published afterwards, it
       could be skipped while already standing in the new space. */
    core_space[me] = space;
    write_cr3(space);
}

uintptr_t tlb_current_space(void)
{
    return read_cr3();
}

/* --- answering -------------------------------------------------------- */

void tlb_service(void)
{
    int me = smp_core();
    uint32_t bit = 1u << me;

    if (!(req_wait & bit))
        return;

    /* Read the request before answering it - after the bit is cleared
       the sender may already be writing the next one. */
    uintptr_t addr = req_addr;
    uintptr_t space = req_space;
    uint32_t kern = req_kernel;

    if (kern || core_space[me] == space) {
        if (addr)
            invlpg(addr);
        else
            write_cr3(read_cr3());     /* the whole space at once */
    }
    clear_bit(&req_wait, bit);
    bump(&stat_acks);
}

void tlb_ipi_recv(void)
{
    smp_tlb_eoi();
    tlb_service();
}

/* --- asking ----------------------------------------------------------- */

static void shootdown(uintptr_t space, uintptr_t addr, int kernel)
{
    int me = smp_core();

    /* Ours first, and without asking anybody: the caller is entitled to
       assume that when this returns, at least ITS OWN core is right. */
    if (kernel || core_space[me] == space) {
        if (addr)
            invlpg(addr);
        else
            write_cr3(read_cr3());
    }

    int cores = smp_started();
    uint32_t mask = 0;
    for (int i = 0; i < cores && i < SMP_CORES; i++) {
        if (i == me)
            continue;
        if (kernel || core_space[i] == space)
            mask |= 1u << i;
    }

    /* Nobody else is holding this space - which is every single-threaded
       process, that is, almost all of them. No interrupt, no waiting,
       nothing to pay. */
    if (!mask) {
        bump(&stat_local);
        return;
    }

    /* Waiting for the slot, we keep answering: a core that stops
       answering while it waits is a core two of them can wait on each
       other forever. */
    while (!try_lock(&busy)) {
        tlb_service();
        relax();
    }

    req_space = space;
    req_addr = addr;
    req_kernel = (uint32_t)kernel;
    __asm__ volatile("" ::: "memory");   /* the mask goes out last */
    req_wait = mask;

    bump(&stat_shootdowns);
    smp_send_tlb();

    while (req_wait) {
        tlb_service();
        relax();
    }
    unlock(&busy);
}

void tlb_invalidate(uintptr_t space, uintptr_t vaddr)
{
    shootdown(space, vaddr & ~(uintptr_t)0xFFF, 0);
}

void tlb_flush_space(uintptr_t space)
{
    shootdown(space, 0, 0);
}

void tlb_flush_kernel(uintptr_t vaddr)
{
    shootdown(0, vaddr & ~(uintptr_t)0xFFF, 1);
}

uint32_t tlb_shootdowns(void) { return stat_shootdowns; }
uint32_t tlb_local_only(void) { return stat_local; }
uint32_t tlb_remote_acks(void) { return stat_acks; }
