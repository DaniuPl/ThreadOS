/* swaptest - swapping pages out to disk
 *
 * It takes more memory than the machine has RAM and writes into it a
 * pattern that depends on the page number. Along the way the kernel has
 * to throw some of the pages out to disk. Then the program reads
 * everything back: every page that went out to disk comes back on the
 * first touch - and has to come back with exactly the same contents.
 *
 * Usage: run swaptest [MiB] (14 by default).
 */

#include "usys.h"

#define PAGE 4096

static unsigned char stamp(uint32_t page)
{
    return (unsigned char)(page * 7 + 13);
}

void _start(int argc, char **argv)
{
    uint32_t mib = 14;
    if (argc > 1) {
        mib = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
            mib = mib * 10 + (uint32_t)(*p - '0');
        if (!mib)
            mib = 14;
    }
    uint32_t bytes = mib * 1024u * 1024u;
    uint32_t pages = bytes / PAGE;

    unsigned char *p = sys_sbrk((int32_t)bytes);
    if (p == (void *)-1) {
        print("[swaptest] sbrk refused\n");
        sys_exit(1);
    }
    p = (unsigned char *)(((uint32_t)p + PAGE - 1) & ~(uint32_t)(PAGE - 1));

    print("[swaptest] writing ");
    print_dec(mib);
    print(" MiB\n");
    for (uint32_t i = 0; i < pages; i++)
        p[i * PAGE] = stamp(i);

    print("[swaptest] reading it back\n");
    uint32_t bad = 0;
    for (uint32_t i = 0; i < pages; i++)
        if (p[i * PAGE] != stamp(i))
            bad++;

    if (bad) {
        print("[swaptest] WRONG: pages with the wrong content ");
        print_dec(bad);
        print("\n");
        sys_exit(1);
    }
    print("[swaptest] all ");
    print_dec(pages);
    print(" pages came back whole\n");
    sys_exit(0);
}
