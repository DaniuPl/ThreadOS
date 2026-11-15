/* deep64 - a stack growing on demand
 *
 * The program gets four pages of stack to start with. This recursion
 * reaches far deeper: every call takes four kilobytes of frame and
 * writes a pattern into it, and on the way back checks whether the
 * pattern survived. If the kernel adds pages under its fingers,
 * everything matches; if not, the program dies at the first crossing of
 * the limit.
 *
 * Checking the pattern ON THE WAY BACK matters here: adding pages on
 * top of overwriting somebody else's frames would look the same
 * halfway.
 */

#include "usys64.h"

#define FRAME 4096
#define DEEP  40

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
    char num[24];

    print("[deep64] I went down to level ");
    utoa(deepest, num);
    print(num);
    print(", frames of 4096 B\n");

    if (bad) {
        print("[deep64] WRONG: the frames did not survive\n");
        sys_exit(1);
    }
    print("[deep64] the stack grew under my fingers, every frame whole\n");
    sys_exit(0);
}
