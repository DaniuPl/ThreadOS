/* heap64 - the heap as seen from a program
 *
 * Until now every buffer had to be hard-coded into the program. This
 * one asks the kernel for memory as it runs and uses it.
 *
 * The most interesting part is what that request costs. The program
 * asks for a quarter of a megabyte and then touches only four pages -
 * and exactly that much memory should be gone, because pages appear
 * only under your fingers. The program does not see that itself; the
 * kernel does, and says so afterwards.
 *
 * It checks itself: if any byte came back different, it ends with an
 * error rather than a message somebody has to read.
 */

#include "usys64.h"

#define REQUEST  (4 * 1024 * 1024)
#define PAGE_MARK  4096
#define TOUCH 4

static char out[128];

static int put(int n, const char *s)
{
    while (*s)
        out[n++] = *s++;
    return n;
}

static int putn(int n, uint64_t v)
{
    char tmp[24];
    utoa(v, tmp);
    return put(n, tmp);
}

static int puth(int n, uint64_t v)
{
    char tmp[24];
    uhtoa(v, tmp);
    return put(n, tmp);
}

static void fail_with(const char *co)
{
    print("[heap64] ERROR: ");
    print(co);
    print("\n");
    sys_exit(1);
}

void _start(void)
{
    void *poczatek = sys_sbrk(0);
    if (!poczatek)
        fail_with("I have no heap");

    int n = put(0, "[heap64] the heap starts at ");
    n = puth(n, (uint64_t)(uintptr_t)poczatek);
    n = put(n, ", asking for ");
    n = putn(n, REQUEST / 1024);
    n = put(n, " KiB (");
    n = putn(n, REQUEST / PAGE_MARK);
    n = put(n, " pages)\n");
    out[n] = 0;
    print(out);

    char *memory = (char *)sys_sbrk(REQUEST);
    if (!memory)
        fail_with("sbrk refused");

    /* We touch only a few pages - the rest is not to exist */
    for (int i = 0; i < TOUCH; i++)
        for (int j = 0; j < PAGE_MARK; j++)
            memory[i * PAGE_MARK + j] = (char)(i * 31 + j);

    for (int i = 0; i < TOUCH; i++)
        for (int j = 0; j < PAGE_MARK; j++)
            if (memory[i * PAGE_MARK + j] != (char)(i * 31 + j))
                fail_with("the memory gives back something other than what was written");

    n = put(0, "[heap64] I touched ");
    n = putn(n, TOUCH);
    n = put(n, " of them, and every one gave back what I wrote\n");
    out[n] = 0;
    print(out);

    /* Shrinking gives the memory back at once */
    if (!sys_sbrk(-REQUEST))
        fail_with("I cannot give the memory back");
    if (sys_sbrk(0) != poczatek)
        fail_with("the break did not come back where it was");
    print("[heap64] I gave it all back, and the break came home\n");

    /* A request beyond our own memory has to be rejected */
    if (sys_sbrk(-4096))
        fail_with("the kernel let me go below the heap");
    print("[heap64] below its own heap the kernel does not let go\n");

    sys_exit(0);
}
