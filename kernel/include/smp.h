#ifndef PROTONOS_SMP_H
#define PROTONOS_SMP_H

#include <stdint.h>

/* How many cores the system supports. Tables indexed by core number
   live in several files, so the count must be visible to all of them. */
#define SMP_CORES 8

/* The remaining processors
 *
 * Until now the system ran on one core and did not even know the
 * others existed. The MADT table in ACPI lists them all, and each of
 * them waits for an INIT-SIPI signal from the boot processor - until
 * it arrives, it just sits there.
 *
 * Woken cores do not run tasks of their own - the scheduler, the
 * process list and the whole rest of the kernel were written for a
 * single core and quietly count on it. What they get instead is
 * countable work: a shared queue of units, from which each takes the
 * next number under a lock and computes it locally. That is the entire
 * state shared between cores - one number and one lock - and that is
 * why it can be reasoned about.
 */

/* Reads the MADT and wakes the remaining processors. Returns how many
   there are in total (including the boot one); 1 = a single-core
   machine or no ACPI. Called after acpi_init and after the heap is up. */
int smp_init(void);

/* How many processors the firmware lists and how many actually reported in */
int smp_cpus(void);
int smp_started(void);

/* The number of the core the caller is running on.
 *
 * The whole kernel asks this, and asks it constantly: the word
 * 'current' means "this core's task", so every use of it is one such
 * question. The answer therefore has to be cheap, and a read from the
 * local APIC is not - it is a hardware register outside the caches,
 * hundreds of cycles every time.
 *
 * So we take the number from the processor's task register: every core
 * has its OWN task state segment and its own selector, and the str
 * instruction reads it from the processor itself, without a single
 * memory access. Before a core loads its segment, str gives zero - and
 * that is the correct answer, because until then only the boot core is
 * running. */
int smp_core(void);

/* The same question asked of the local APIC. It costs a hardware
   register read, but it is the only answer a core has before it loads
   its task state segment - and that is exactly what it is for. */
int smp_core_apic(void);

/* Ticks of a given core's own timer. The boot core runs on the PIT and
   has zero here; secondary ones count their own APIC timers. */
uint32_t smp_ticks(int core);

/* Handling of an APIC timer tick (vector 48) - called by isr_handler */
void smp_timer_tick(void);

/* Sends the shootdown signal (vector 50) to all the other cores: a
   mapping has changed and they may still be holding the old one. Who
   is actually waited for is decided by kernel/tlb.c. */
void smp_send_tlb(void);

/* Acknowledges the shootdown interrupt to this core's own APIC */
void smp_tlb_eoi(void);

/* Handling of a wake-up from another core (vector 49). It does nothing
   beyond acknowledging: its entire content is the fact that it arrived
   and pulled the processor out of the hlt instruction. */
void smp_wakeup_recv(void);

/* Runs whatever is left of the smp_work order (or nothing, if there is
   no order). Called from a core's idle loop. */
void smp_run_work(int core);

/* --- Work spread across cores ---------------------------------------
 *
 * Orders 'count' units of work and returns once all of them are
 * computed. Each unit gets its number; what it does with it is the
 * function's business. The boot core computes along with the rest -
 * otherwise on a single-core machine nobody would compute at all.
 *
 * The function runs on a core with interrupts off and without an
 * exception table: it may not sleep, may not call into the kernel and
 * may not get an address wrong. It is to compute what it was given,
 * and that is all. */
void smp_work(void (*fn)(uint32_t number, void *data), void *data,
               uint32_t count);

/* How many units each core computed in the last order (index 0 is the
   boot core) and how many cores took part in it at all */
uint32_t smp_share(int core);
int smp_workers(void);

#endif
