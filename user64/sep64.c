/* sep64 - proof that every process has its own address space
 *
 * The kernel starts two copies of this program at once. Both are the
 * same file, loaded at the same virtual address, and both write to the
 * same address of their variable. One thing tells them apart: the
 * number they were given by the kernel in the RDI register.
 *
 * If the space were shared, the second copy would overwrite the first
 * one's value - and the first would notice. The program writes its
 * number, hands the processor over to the other one for a moment, and
 * then checks whether its memory is as it left it.
 */

#include "usys64.h"

static volatile uint64_t marker;     /* the same address in both processes */
static char buf[96];

static int put(int n, const char *s)
{
    while (*s)
        buf[n++] = *s++;
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

void _start(int argc, char **argv)
{
    /* The number arrives as an ordinary argument - the way every
       program started from a command line gets it */
    int64_t id = argc > 1 ? uatoi(argv[1]) : 0;
    uint64_t mine = 1000 + (uint64_t)id;
    marker = mine;

    int n = put(0, "[sep64] process ");
    n = putn(n, (uint64_t)id);
    n = put(n, ": address ");
    n = puth(n, (uint64_t)(uintptr_t)&marker);
    n = put(n, ", I wrote ");
    n = putn(n, mine);
    buf[n++] = '\n';
    buf[n] = 0;
    print(buf);

    /* A moment for the other process - let it write its own */
    sys_sleep(200);

    uint64_t now = marker;
    n = put(0, "[sep64] process ");
    n = putn(n, (uint64_t)id);
    n = put(n, now == mine ? ": still " : ": ERROR, now ");
    n = putn(n, now);
    n = put(n, now == mine ? " - my memory is untouched\n"
                           : " - somebody overwrote it\n");
    buf[n] = 0;
    print(buf);

    sys_exit(now == mine ? 0 : 1);
}
