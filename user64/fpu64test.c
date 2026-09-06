/* fpu64test - floating-point numbers and the SSE registers
 *
 * The kernel starts two copies at once, each with a different starting
 * number. Both count in a loop, handing over the processor along the
 * way - and if the floating-point registers were not saved on a task
 * switch, each would get the other one's result.
 *
 * The operation is chosen so as to be exact: v = v * 2 + 1 on integers
 * that fit in double precision loses not a single bit, so the result
 * can be checked down to the unit. After twenty turns, 'a' comes out as
 * (a + 1) * 2^20 - 1.
 */

#include "usys64.h"

#define SPINS 20

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

void _start(int argc, char **argv)
{
    int64_t start = argc > 1 ? uatoi(argv[1]) : 1;
    if (start < 0)
        start = 1;

    double v = (double)start;
    for (int i = 0; i < SPINS; i++) {
        v = v * 2.0 + 1.0;
        sys_sleep(20);               /* this is where the other process steps in */
    }

    /* The same on integers - the result has to be identical */
    uint64_t expected = (uint64_t)start;
    for (int i = 0; i < SPINS; i++)
        expected = expected * 2 + 1;

    uint64_t result = (uint64_t)v;

    int n = put(0, "[fpu64] start ");
    n = putn(n, (uint64_t)start);
    n = put(n, ": came out ");
    n = putn(n, result);
    n = put(n, ", wanted ");
    n = putn(n, expected);
    n = put(n, result == expected ? " - it matches\n"
                                   : " - IT DOES NOT MATCH\n");
    out[n] = 0;
    print(out);

    sys_exit(result == expected ? 0 : 1);
}
