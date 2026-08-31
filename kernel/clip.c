/* The clipboard: text between two windows
 *
 * A buffer in the kernel and two ways to reach it. What makes it worth
 * having is not the code - it is that both sides of the copy are ring
 * 3 programs that know nothing about each other, and this is the only
 * ground they share.
 *
 * The kernel is the owner, so the text survives the program that put
 * it there: closing the terminal you copied from does not empty the
 * clipboard, exactly as it should not.
 */

#include "include/clip.h"

static char text[CLIP_MAX + 1];
static uint32_t length;

uint32_t clip_set(const char *src, uint32_t len)
{
    if (len > CLIP_MAX)
        len = CLIP_MAX;
    for (uint32_t i = 0; i < len; i++)
        text[i] = src[i];
    text[len] = 0;
    length = len;
    return len;
}

uint32_t clip_get(char *out, uint32_t max)
{
    if (!max)
        return 0;
    uint32_t n = length;
    if (n > max - 1)
        n = max - 1;
    for (uint32_t i = 0; i < n; i++)
        out[i] = text[i];
    out[n] = 0;
    return n;
}

uint32_t clip_len(void)
{
    return length;
}
