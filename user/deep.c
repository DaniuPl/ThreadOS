/* deep - a stack growing on demand
 *
 * The program gets eight pages of stack to start with. This recursion
 * reaches far deeper: every call takes four kilobytes of frame and
 * writes a pattern into it, and on the way back checks whether the
 * pattern survived. If the kernel added pages under its fingers,
 * everything matches; if not, the program dies at the first crossing of
 * the limit.
 *
 * Checking the pattern ON THE WAY BACK matters here: adding pages on
 * top of overwriting somebody else's frames would look the same
 * halfway.
 */

#include "usys.h"

#define FRAME   4096
#define DEEP 40

static uint32_t deepest;

static uint32_t descend(uint32_t level)
{
    volatile unsigned char frame[FRAME];
    for (uint32_t i = 0; i < FRAME; i += 512)
        frame[i] = (unsigned char)(level + i);

    deepest = level;
    uint32_t below = level < DEEP ? descend(level + 1) : 0;

    /* On the way back: is the frame still ours and untouched? */
    for (uint32_t i = 0; i < FRAME; i += 512)
        if (frame[i] != (unsigned char)(level + i))
            return below + 1000000;
    return below;
}

void _start(void)
{
    uint32_t bad = descend(1);

    print("[deep] zeszlem na poziom ");
    print_dec(deepest);
    print(", ramek po ");
    print_dec(FRAME);
    print(" B\n");

    if (bad) {
        print("[deep] ZLE: ramki nie przetrwaly\n");
        sys_exit(1);
    }
    print("[deep] stos urosl pod palcami, wszystkie ramki caly\n");
    sys_exit(0);
}
