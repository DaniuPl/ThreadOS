/* tlbtest - threads of one process on several cores at once
 *
 * Until now a process ran on one core at a time, and that hid a whole
 * class of mistakes: a mapping changed on one core said nothing about
 * the translation the others had already worked out for themselves.
 * Threads share an address space, so they are the first thing to walk
 * into it.
 *
 * Every thread asks for fresh memory in every round and writes a
 * pattern into it. Fresh memory is the point: each of those pages is a
 * new entry in the tables of a space three other cores are standing in
 * at that very moment, and every one of them is a change somebody else
 * may already be holding an answer to.
 *
 * The pattern is checked twice. Once by the thread that wrote it, and
 * once at the end by the main thread, over EVERY chunk of every
 * thread - and that second one is the one that matters. A frame handed
 * out twice, or a translation left over from before a change, shows up
 * there and not in the first check: by then the neighbours have had
 * time to be given memory somebody else was still writing to.
 *
 * There has to be more work than one core can carry, or the whole
 * thing passes without ever leaving the boot core and proves nothing.
 * Hence the rounds, and hence the mask of cores every thread records
 * for itself: without it the run says nothing about more than one.
 *
 * Usage: run tlbtest    - all threads on the boot core
 *        runw tlbtest   - they are allowed to run anywhere
 */

#include "usys.h"

#define THREADS    4
#define ROUNDS    10
#define CHUNK      4                  /* pages asked for in one round */
#define PAGE    4096
#define STRIDE    64                  /* every 64th byte carries the pattern */

static volatile uint32_t seen_cores[THREADS];   /* cores that carried it */
static volatile uint32_t bad_early[THREADS];    /* mismatches it saw itself */
static volatile uint32_t standing[THREADS];     /* the thread is up */
static volatile uint32_t go;                    /* all of them are - start */
static volatile uint32_t chunks[THREADS][ROUNDS];  /* where each one landed */

/* The byte of chunk 'id' at offset 'i'. It depends on both, so a page
   put in the wrong place is as visible as a page with wrong contents. */
static unsigned char want(uint32_t id, uint32_t i)
{
    return (unsigned char)((id * 31u + i * 7u + 11u) & 0xFF);
}

static uint32_t check_chunk(unsigned char *p, uint32_t id)
{
    uint32_t bad = 0;
    for (uint32_t i = 0; i < CHUNK * PAGE; i += STRIDE)
        if (p[i] != want(id, i))
            bad++;
    return bad;
}

static void worker(void *arg)
{
    uint32_t me = (uint32_t)(uintptr_t)arg;
    uint32_t bad = 0;

    standing[me] = 1;
    while (!go)
        sys_sleep(1);

    for (uint32_t round = 0; round < ROUNDS; round++) {
        uint32_t id = me * ROUNDS + round;
        unsigned char *p = (unsigned char *)sys_mmap(CHUNK * PAGE);
        if (!p) {
            bad++;
            break;
        }
        chunks[me][round] = (uint32_t)(uintptr_t)p;

        /* The first touch of every page is what makes the kernel map
           it - and it maps it into a space the others are standing in */
        for (uint32_t i = 0; i < CHUNK * PAGE; i += STRIDE)
            p[i] = want(id, i);
        bad += check_chunk(p, id);

        /* Enough work that one core cannot quietly carry all four
           threads: without that the run never leaves the boot core */
        volatile uint32_t sum = 0;
        for (uint32_t rep = 0; rep < 40; rep++)
            for (uint32_t i = 0; i < CHUNK * PAGE; i += 4)
                sum += p[i];

        seen_cores[me] |= 1u << (uint32_t)sys_core();
    }

    bad_early[me] = bad;
    sys_exit(0);
}

void _start(void)
{
    int32_t tid[THREADS];
    for (uint32_t i = 0; i < THREADS; i++) {
        tid[i] = sys_thread(worker, (void *)(uintptr_t)i);
        if (tid[i] < 0) {
            print("[tlbtest] the thread could not be created\n");
            sys_exit(1);
        }
    }

    /* Let them all start together: a thread that finishes before the
       next one is created never meets anybody */
    for (;;) {
        uint32_t up = 0;
        for (uint32_t i = 0; i < THREADS; i++)
            up += standing[i];
        if (up == THREADS)
            break;
        sys_sleep(1);
    }
    go = 1;

    uint32_t cores = 0, bad = 0;
    for (uint32_t i = 0; i < THREADS; i++) {
        sys_wait(tid[i]);
        cores |= seen_cores[i];
        bad += bad_early[i];
    }

    /* The late check: every chunk of every thread, read once all of
       them have finished. What survives here survived the neighbours. */
    uint32_t late = 0, seen = 0;
    for (uint32_t t = 0; t < THREADS; t++)
        for (uint32_t r = 0; r < ROUNDS; r++) {
            unsigned char *p = (unsigned char *)(uintptr_t)chunks[t][r];
            if (!p)
                continue;
            late += check_chunk(p, t * ROUNDS + r);
            seen++;
        }

    print("[tlbtest] threads ");
    print_dec(THREADS);
    print(", chunks ");
    print_dec(seen);
    print(" of ");
    print_dec(CHUNK);
    print(" pages, core mask ");
    print_dec(cores);
    print("\n[tlbtest] errors while writing ");
    print_dec(bad);
    print(", afterwards ");
    print_dec(late);
    print("\n");

    if (bad || late || seen != THREADS * ROUNDS) {
        print("[tlbtest] WRONG: the memory does not match\n");
        sys_exit(2);
    }
    print("[tlbtest] the memory agrees on every core\n");
    sys_exit(0);
}
