/* pulook - ProtonUI v2's look, as SYS64_UI_LOOK hands it out
 *
 * t_protonui (the PC) and t_arm64win (the phone) read what this prints
 * and compare it with kernel/include/look.h. Every array is filled with
 * a guard word first, so what the kernel wrote and what it left alone
 * are both seen:
 *
 *   LOOK v1 <64 words>     the call as every program before v2 makes
 *                          it: eleven words, nothing past them
 *   LOOK v2 <64 words>     22 words asked for (UI_LOOK_MAX)
 *   LOOK big <64 words>    64 asked for: still no more than 22
 *   LOOK night <64 words>  the dark theme's, 22
 *   LOOK linen <64 words>  the light theme's, 22
 *
 * and "[pulook] done". The raw call, not user/udraw.h, so that the
 * same file is built for the phone.
 */

#include <stdint.h>
#include <stdio.h>
#include "usys64.h"

#define WORDS 64
#define GUARD 0xA5A5A5A5u

static uint32_t look[WORDS];

/* SYS64_UI_LOOK's second argument: the theme in its low byte, how many
   words are wanted in the next one (kernel/include/desktop.h) */
static void ask(const char *what, int theme, int words)
{
    for (int i = 0; i < WORDS; i++)
        look[i] = GUARD;
    syscall64(SYS64_UI_LOOK, (int64_t)(uintptr_t)look,
              (theme & 0xFF) | (words & 0xFF) << 8);
    printf("LOOK %s", what);
    for (int i = 0; i < WORDS; i++)
        printf(" %08x", (unsigned)look[i]);
    printf("\n");
}

int main(void)
{
    ask("v1", 0, 0);
    ask("v2", 0, 22);
    ask("big", 0, WORDS);
    ask("night", 1, 22);
    ask("linen", 2, 22);
    printf("[pulook] done\n");
    return 0;
}
