#ifndef THREADOS_UBYTES_H
#define THREADOS_UBYTES_H

#include <stdint.h>
#include "ulocale.h"

static int format_bytes(uint64_t bytes, char *out, int size)
{
    static const char *const units[] = {
        "B", "KiB", "MiB", "GiB", "TiB"
    };
    uint64_t divisor = 1;
    int unit = 0;

    while (unit < 4 && bytes >= divisor * 1024) {
        divisor *= 1024;
        unit++;
    }

    uint64_t tenths = bytes * 10 / divisor;
    uint64_t remainder = bytes * 10 % divisor;
    if (remainder >= (divisor + 1) / 2)
        tenths++;
    if (unit < 4 && tenths >= 10240) {
        divisor *= 1024;
        unit++;
        tenths = bytes * 10 / divisor;
        remainder = bytes * 10 % divisor;
        if (remainder >= (divisor + 1) / 2)
            tenths++;
    }

    char digits[24];
    int n = 0, used = 0;
    uint64_t whole = tenths / 10;
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
