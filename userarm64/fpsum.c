/* fpsum SEED - a long sum in floating point that never gives the core up
 *
 * Two of them on one core are interleaved only by the timer, and each
 * comes out right only if every switch between them carried its FP/SIMD
 * registers along (kernelarm64/thread.c). The test sums the same in
 * Python (IEEE doubles, the same order) and compares the bits.
 */
#include "usys.h"

int main(int argc, char **argv)
{
    int seed = 0;
    for (const char *c = argc > 1 ? argv[1] : "1"; *c >= '0' && *c <= '9';
         c++)
        seed = seed * 10 + (*c - '0');
    int64_t began = sys_ticks();
    double x = seed, acc = 0;
    for (int i = 0; i < 3000000; i++) {
        x = x * 1.0000001 + 0.5;
        if (x > 1e6)
            x = x / 3.0;
        acc = acc + x;
    }
    union { double d; uint64_t u; } bits = { acc };
    /* With when it ran, in timer ticks: two that overlap were
       interleaved */
    struct line l = { .n = 0 };
    line_add(&l, "fpsum ");
    line_dec(&l, seed);
    line_add(&l, " ");
    line_hex(&l, bits.u);
    line_add(&l, " ticks ");
    line_dec(&l, began);
    line_add(&l, " ");
    line_dec(&l, sys_ticks());
    line_out(&l);
    return 0;
}
