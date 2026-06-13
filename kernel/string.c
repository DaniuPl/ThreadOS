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

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    while (n--) {
        if (*p != *q)
            return *p - *q;
        p++;
        q++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len])
        len++;
    return len;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
