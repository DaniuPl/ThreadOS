/* tlbtest64 - threads of one process on several cores (long mode)
 *
 * The same test as user/tlbtest.c and for the same reason, only on the
 * other side. Until now the long-mode kernel had no threads at all, so
 * two cores could not be put into ONE address space there - and the
 * shootdown could be built into it but not shown working.
 *
 * Every thread asks for fresh memory in every round and writes a
 * pattern into it. Fresh memory is the point: each of those pages is a
 * new entry in the tables of a space three other cores are standing in
 * at that very moment.
 *
 * The pattern is checked twice - by the thread that wrote it, and at
 * the end by the main thread over EVERY chunk. The second check is the
 * one that matters: by then the neighbours have had time to be handed
 * memory somebody else was still writing to.
 *
 * Usage: run tlbtest64    - all threads on the boot core
 *        runw tlbtest64   - they are allowed to run anywhere
 */

#include "usys64.h"

#define THREADS    4
#define ROUNDS    10
#define CHUNK      4                  /* pages asked for in one round */
#define PAGE    4096
#define STRIDE    64                  /* every 64th byte carries the pattern */

static volatile uint32_t seen_cores[THREADS];
static volatile uint32_t bad_early[THREADS];
static volatile uint32_t standing[THREADS];
static volatile uint32_t go;
static volatile uint64_t chunks[THREADS][ROUNDS];

static void put_num(uint64_t v)
{
    char buf[24];
    utoa(v, buf);
    print(buf);
}

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
        chunks[me][round] = (uint64_t)(uintptr_t)p;

        /* The first touch of every page is what makes the kernel map
           it - into a space the others are standing in */
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
    for (uint32_t i = 0; i < THREADS; i++)
        if (sys_thread(worker, (void *)(uintptr_t)i) < 0) {
            print("[tlbtest64] the thread could not be created\n");
            sys_exit(1);
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

    /* sys_wait takes any of our own children, and threads are all we
       have - so counting them is enough */
    for (uint32_t i = 0; i < THREADS; i++) {
        int code = 0;
        sys_wait(&code);
    }

    uint32_t cores = 0, bad = 0;
    for (uint32_t i = 0; i < THREADS; i++) {
        cores |= seen_cores[i];
        bad += bad_early[i];
    }

    uint32_t late = 0, seen = 0;
    for (uint32_t t = 0; t < THREADS; t++)
        for (uint32_t r = 0; r < ROUNDS; r++) {
            unsigned char *p = (unsigned char *)(uintptr_t)chunks[t][r];
            if (!p)
                continue;
            late += check_chunk(p, t * ROUNDS + r);
            seen++;
        }

    print("[tlbtest64] threads ");
    put_num(THREADS);
    print(", chunks ");
    put_num(seen);
    print(" of ");
    put_num(CHUNK);
    print(" pages, core mask ");
    put_num(cores);
    print("\n[tlbtest64] errors while writing ");
    put_num(bad);
    print(", afterwards ");
    put_num(late);
    print("\n");

    if (bad || late || seen != THREADS * ROUNDS) {
        print("[tlbtest64] WRONG: the memory does not match\n");
        sys_exit(2);
    }
    print("[tlbtest64] the memory agrees on every core\n");
    sys_exit(0);
}
