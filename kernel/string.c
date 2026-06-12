/* Minimalne funkcje z biblioteki standardowej - w środowisku freestanding
 * nie mamy libc, a GCC i tak może emitować wywołania memset/memcpy. */

#include "include/string.h"

void *memset(void *dst, int value, size_t n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)value;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len])
        len++;
    return len;
}
