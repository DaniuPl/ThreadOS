/* Kernel threads on all cores - a trial
 *
 * A task created by task_create_anycore may be run by any core. An
 * ordinary task stays on the boot one, and rightly so: it reaches for
 * drivers and the filesystem, and those were written for a single
 * core. Here the caller takes on that its thread touches only what
 * survives two cores at once - and this one touches its own cell in a
 * table and nothing more.
 *
 * The proof has two parts, because each on its own could be faked. The
 * core number is written down by the thread itself, while computing -
 * but if the scheduler merely shuffled it between cores in turn, the
 * mask would look exactly the same. So we also measure time: the same
 * work run in parallel is to finish sooner than the same work done one
 * piece after another. A single core cannot fake that.
 */

#include <stdint.h>
#include "include/smpthreads.h"
#include "include/smp.h"
#include "include/task.h"
#include "include/pit.h"
#include "include/printk.h"

#define THREADS   4
#define STEPS   100000000u

static volatile uint32_t mask[THREADS];    /* the cores that ran it */
static volatile uint32_t result[THREADS];
static volatile uint32_t finished;

/* A linear congruential generator - always the same sequence for a
   given number, so the result does not depend on who computed it */
static uint32_t count(uint32_t number, volatile uint32_t *where_i_was)
{
    uint32_t stan = number * 2654435761u + 12345u;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < STEPS; i++) {
        stan = stan * 1103515245u + 12345u;
        sum += stan >> 16;
        /* Now and then write down which core we are on right now.
           Rarely, because reading the number goes through the local
           APIC. */
        if (where_i_was && (i & 0xFFFF) == 0)
            *where_i_was |= 1u << smp_core();
    }
    return sum;
}

static void thread(void *arg)
{
    uint32_t i = (uint32_t)(uintptr_t)arg;
    result[i] = count(0, &mask[i]);
    __atomic_add_fetch(&finished, 1, __ATOMIC_SEQ_CST);
}

void smpthreads_test(void)
{
    for (int i = 0; i < THREADS; i++) {
        mask[i] = 0;
        result[i] = 0;
    }
    finished = 0;

    /* --- how long ONE portion takes, computed here, in the shell thread ---
       This is the reference measurement. Four portions run in parallel
       are to take the same time as one; done one after another they
       would take four times as long. Between the one and the other
       lies the whole difference. */
    uint32_t start = pit_ticks();
    uint32_t pattern = count(0, (volatile uint32_t *)0);
    uint32_t time_one = pit_ticks() - start;

    /* --- four portions at once, each in its own thread --- */
    start = pit_ticks();
    for (uint32_t i = 0; i < THREADS; i++)
        task_create_anycore("counting", thread, (void *)(uintptr_t)i);
    while (__atomic_load_n(&finished, __ATOMIC_SEQ_CST) < THREADS)
        task_sleep(10);
    uint32_t time_four = pit_ticks() - start;

    uint32_t all_cores = 0;
    int matches = 1;
    for (int i = 0; i < THREADS; i++) {
        all_cores |= mask[i];
        /* Every thread computed the same portion as the reference
           thread - the result is to be the same to the bit, no matter
           which core computed it */
        if (result[i] != pattern)
            matches = 0;
        printk("  thread%d: cores %x, result %u\n", i, mask[i], result[i]);
    }

    printk("Threads: %u of %u steps, cores in the machine: %d\n",
           (uint32_t)THREADS, STEPS, smp_cpus());
    printk("The cores that ran them: %x\n", all_cores);
    printk("Result: %s\n", matches ? "matches" : "DOES NOT MATCH");
    printk("One batch: %u ticks, four at once: %u ticks\n",
           time_one, time_four);
    printk("Threads off the boot core: %s\n",
           (all_cores & ~1u) ? "yes" : "NO");
}
