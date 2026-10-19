#ifndef THREADOS_ULOCALE_H
#define THREADOS_ULOCALE_H

#include "uapi.h"

static int ulocale_decimal_comma(void)
{
    struct cfg_entry entry;
    for (uint32_t i = 0; sys_cfg_get(i, &entry) == 0; i++) {
        if (entry.name[0] == 'n' && entry.name[1] == 'u' &&
            entry.name[2] == 'm' && entry.name[3] == 'f' &&
            entry.name[4] == 'm' && entry.name[5] == 't' &&
            !entry.name[6])
            return entry.value[0] == 'c' && entry.value[1] == 'o' &&
                   entry.value[2] == 'm' && entry.value[3] == 'm' &&
                   entry.value[4] == 'a' && !entry.value[5];
    }
    return 0;
}

#endif
