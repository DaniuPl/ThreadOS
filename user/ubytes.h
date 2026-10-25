#ifndef THREADOS_UBYTES_H
#define THREADOS_UBYTES_H

#include <stdint.h>
#include "ulocale.h"

static int format_bytes(uint64_t bytes, char *out, int size)
{
    static const char *const units[] = {
        "B", "KiB", "MiB", "GiB", "TiB"
    };
    /* Shifts and masks, not division: the units are powers of two, and
       a 64-bit division in protected mode would need libgcc, which the
       programs are not linked with */
    int shift = 0;
    int unit = 0;

    while (unit < 4 && (bytes >> shift) >= 1024) {
        shift += 10;
        unit++;
    }

    uint64_t mask = shift ? (((uint64_t)1 << shift) - 1) : 0;
    uint64_t half = shift ? ((uint64_t)1 << (shift - 1)) : 1;
    uint32_t tenths = (uint32_t)((bytes * 10) >> shift);
    if (shift && ((bytes * 10) & mask) >= half)
        tenths++;
    if (unit < 4 && tenths >= 10240) {
        shift += 10;
        unit++;
        mask = ((uint64_t)1 << shift) - 1;
        half = (uint64_t)1 << (shift - 1);
        tenths = (uint32_t)((bytes * 10) >> shift);
        if (((bytes * 10) & mask) >= half)
            tenths++;
    }

    char digits[24];
    int n = 0, used = 0;
    uint32_t whole = tenths / 10;
    do {
        digits[n++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole && n < (int)sizeof(digits));

    while (n && used < size - 1)
        out[used++] = digits[--n];
    if (unit == 0) {
        if (used < size - 1)
            out[used++] = ' ';
    } else if (tenths % 10 && used < size - 1) {
        out[used++] = ulocale_decimal_comma() ? ',' : '.';
        if (used < size - 1)
            out[used++] = (char)('0' + tenths % 10);
        else
            used--;
    }
    if (used < size - 1)
        out[used++] = ' ';
    for (const char *p = units[unit]; *p && used < size - 1; p++)
        out[used++] = *p;
    if (size > 0)
        out[used] = '\0';
    return used;
}

#endif
