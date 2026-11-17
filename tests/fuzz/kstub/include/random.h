/* Stand-in for the kernel's random.h in the host harness: the numbers
 * only have to differ from one another, and the same run gives the same
 * ones, so a failing case can be repeated */
#ifndef KSTUB_RANDOM_H
#define KSTUB_RANDOM_H

#include <stdint.h>

static inline void random_bytes(void *out, uint32_t n)
{
    static uint64_t x = 0x9E3779B97F4A7C15ull;
    uint8_t *p = out;
    while (n--) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        *p++ = (uint8_t)x;
    }
}

#endif
