/* Minimal standard library functions - in a freestanding environment
 * we have no libc, and GCC may emit calls to memset/memcpy anyway. */

#include <stdint.h>
#include "include/string.h"

/* memset, memcpy and memcmp carry whole frames now - a screen is a
 * megabyte and more, several times a frame - so they go a word at a
 * time. The string instructions do it for the first two (with the
 * direction flag cleared first: a program may have set it before a
 * system call, and backwards would be a disaster); the comparison is a
 * plain loop over words, the bytes only for what does not fill one. */

void *memset(void *dst, int value, size_t n)
{
    void *d = dst;
    uint32_t v = (uint8_t)value * 0x01010101u;
    size_t words = n / 4, rest = n % 4;
    __asm__ volatile("cld; rep stosl" : "+D"(d), "+c"(words) : "a"(v)
                     : "memory");
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(rest) : "a"(v) : "memory");
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    void *d = dst;
    size_t words = n / 4, rest = n % 4;
    __asm__ volatile("cld; rep movsl" : "+D"(d), "+S"(src), "+c"(words)
                     : : "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(rest)
                     : : "memory");
    return dst;
}

typedef unsigned long __attribute__((may_alias)) word_t;

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    if ((((uintptr_t)p | (uintptr_t)q) & (sizeof(word_t) - 1)) == 0) {
        while (n >= sizeof(word_t) &&
               *(const word_t *)p == *(const word_t *)q) {
            p += sizeof(word_t);
            q += sizeof(word_t);
            n -= sizeof(word_t);
        }
    }
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
