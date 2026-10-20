#ifndef THREADOS_ULOCALE_H
#define THREADOS_ULOCALE_H

#include "uapi.h"

static int ulocale_language_polish(void)
{
    static int cached = -1;
    static uint32_t next_check;
    uint32_t now = sys_getticks();
    if (cached >= 0 && (int32_t)(now - next_check) < 0)
        return cached;

    struct cfg_entry entry;
    for (uint32_t i = 0; sys_cfg_get(i, &entry) == 0; i++) {
        if (entry.name[0] == 'l' && entry.name[1] == 'a' &&
            entry.name[2] == 'n' && entry.name[3] == 'g' &&
            entry.name[4] == 'u' && entry.name[5] == 'a' &&
            entry.name[6] == 'g' && entry.name[7] == 'e' &&
            !entry.name[8]) {
            cached = entry.value[0] == 'p' && entry.value[1] == 'l' &&
                     !entry.value[2];
            next_check = now + 50;
            return cached;
        }
    }
    cached = 0;
    next_check = now + 50;
    return cached;
}

static inline void ulocale_window_title(int win, const char *english,
                                        const char *polish)
{
    int language = ulocale_language_polish();
    static int current_language = -1;
    if (language == current_language)
        return;
    win_title(win, language ? polish : english);
    current_language = language;
}

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
