/* swaptest64 - swapping pages out to disk as seen from a program
 *
 * It takes more memory than the machine has RAM and writes into it a
 * pattern that depends on the page number. Along the way the kernel has
 * to throw some of the pages out to disk. Then the program reads
 * everything back: every page that went out to disk comes back on the
 * first touch - and has to come back with exactly the same contents.
 *
 * The program knows nothing about swapping and that is exactly the
 * point: to it, memory is simply memory. It checks itself - if even one
 * byte came back different, it ends with an error rather than a message
 * somebody would have to read.
 *
 * Usage: run swaptest64 [MiB] (14 by default).
 */

#include "usys64.h"

#define PAGE_MARK 4096

static char out[96];

static void report(const char *a, uint64_t v, const char *b)
{
    int n = 0;
    while (*a)
        out[n++] = *a++;
    char tmp[24];
    utoa(v, tmp);
    for (int i = 0; tmp[i]; i++)
        out[n++] = tmp[i];
    while (*b)
        out[n++] = *b++;
    out[n] = '\0';
    print(out);
}

static unsigned char tag(uint64_t page)
{
    return (unsigned char)(page * 7 + 13);
}

void _start(int argc, char **argv)
{
    uint64_t mib = 14;
    if (argc > 1) {
        int64_t v = uatoi(argv[1]);
        if (v > 0)
            mib = (uint64_t)v;
    }
    uint64_t nbytes = mib * 1024u * 1024u;
    uint64_t pages = nbytes / PAGE_MARK;

    unsigned char *p = sys_sbrk((int64_t)nbytes);
    if (!p) {
        print("[swaptest64] sbrk odmowil\n");
        sys_exit(1);
    }
    p = (unsigned char *)(((uintptr_t)p + PAGE_MARK - 1) & ~(uintptr_t)(PAGE_MARK - 1));

    report("[swaptest64] zapisuje ", mib, " MiB\n");
    for (uint64_t i = 0; i < pages; i++)
        p[i * PAGE_MARK] = tag(i);

    print("[swaptest64] czytam z powrotem\n");
    uint64_t bad = 0;
    for (uint64_t i = 0; i < pages; i++)
        if (p[i * PAGE_MARK] != tag(i))
            bad++;

    if (bad) {
        report("[swaptest64] ZLE: stron z bledna zawartoscia ", bad, "\n");
        sys_exit(1);
    }
    report("[swaptest64] wszystkie ", pages, " stron wrocilo w calosci\n");
    sys_exit(0);
}
