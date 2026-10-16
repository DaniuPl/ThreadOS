#ifndef THREADOS_TLB_H
#define THREADOS_TLB_H

#include <stdint.h>

/* TLB shootdown - telling the other cores that a mapping has changed
 *
 * The processor remembers translations it has already worked out, and
 * it remembers them PER CORE. Changing a page table changes what the
 * memory says; it does not change what another core has cached. So a
 * core that has read the old entry keeps writing to the old frame -
 * one the kernel may already have freed, handed to somebody else, or
 * copied away from.
 *
 * Until now nothing suffered from this, and for one reason only: one
 * process ran on one core at a time. Threads sharing an address space
 * are the first thing that breaks it, and they are exactly what this
 * makes possible.
 *
 * There is no instruction that invalidates another core's entry. The
 * only way is to ask: send an interrupt, and WAIT for the answer. The
 * waiting is what makes this a shootdown and not a hint - after
 * tlb_invalidate() returns, no core can reach that page through the
 * old translation any more, so the frame is safe to free.
 *
 * The cost is paid only where it is owed. Every core says which space
 * it is running, so a change in a space nobody else holds sends no
 * interrupt at all - which is every single-threaded process, that is,
 * almost all of them.
 */

/* Writes CR3 and says so. Everything that changes an address space
   goes through here: a value the kernel publishes and a value the
   processor holds must not be able to drift apart. */
void tlb_switch(uintptr_t space);

/* The space this core is running (its CR3) */
uintptr_t tlb_current_space(void);

/* One page of one address space has changed. Invalidates it here and
   on every other core running that space, and returns once they have
   all confirmed it. */
void tlb_invalidate(uintptr_t space, uintptr_t vaddr);

/* The whole space has changed (many pages at once, or its tables are
   about to be destroyed). Whoever runs it reloads CR3. */
void tlb_flush_space(uintptr_t space);

/* A kernel mapping has changed. Kernel tables are shared by every
   space, so this concerns every core regardless of what it is
   running. */
void tlb_flush_kernel(uintptr_t vaddr);

/* Handling of the shootdown interrupt (vector 50) - called by
   isr_handler */
void tlb_ipi_recv(void);

/* Handles a request aimed at this core, if one is waiting. Called from
   every loop that waits with interrupts off: without it two cores
   could sit waiting for each other's answer. */
void tlb_service(void);

/* --- what it cost ----------------------------------------------------
 *
 * Nothing here is needed to make it work; it is here so the test can
 * ask. The interesting number is not how many pages changed but how
 * many of those changes had to reach another core at all. */
uint32_t tlb_shootdowns(void);   /* changes that needed an interrupt */
uint32_t tlb_local_only(void);   /* changes nobody else had to hear about */
uint32_t tlb_remote_acks(void);  /* answers collected from other cores */

#endif
