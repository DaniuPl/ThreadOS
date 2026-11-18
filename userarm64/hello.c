/* hello - the first program of the ARM64 kernel's
 *
 * It says what it was given, then does what a program does with its
 * memory, each with an answer the test works out on its own in Python
 * (scripts/tests/t_arm64user.py): a megabyte of its .bss filled and
 * hashed, four megabytes of heap from SYS64_SBRK filled and hashed, and
 * a sum in floating point that only the FP/SIMD registers compute.
 */
#include "usys.h"

static uint8_t big[1 << 20];

/* FNV-1a, 64 bits */
static uint64_t fnv(const uint8_t *p, uint64_t n)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint64_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ULL;
    return h;
}

int main(int argc, char **argv)
{
    struct line l = { .n = 0 };
    line_add(&l, "hello from EL0, pid ");
    line_dec(&l, sys_getpid());
    line_add(&l, ", args");
    for (int i = 0; i < argc; i++) {
        line_add(&l, " ");
        line_add(&l, argv[i]);
    }
    line_out(&l);

    for (uint64_t i = 0; i < sizeof(big); i++)
        big[i] = (uint8_t)(i * 7 + (i >> 9));
    line_add(&l, "bss ");
    line_hex(&l, fnv(big, sizeof(big)));
    line_out(&l);

    uint8_t *heap = sys_sbrk(4 << 20);
    for (uint64_t i = 0; i < (4u << 20); i++)
        heap[i] = (uint8_t)(i ^ (i >> 11));
    line_add(&l, "heap ");
    line_hex(&l, fnv(heap, 4 << 20));
    line_out(&l);

    double s = 0;
    for (int k = 1; k <= 100000; k++)
        s += 1.0 / ((double)k * (double)k);
    union { double d; uint64_t u; } bits = { s };
    line_add(&l, "fp ");
    line_hex(&l, bits.u);
    line_out(&l);
    return 7;
}
