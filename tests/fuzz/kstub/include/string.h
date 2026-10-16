/* A stub for the kernel's string.h: the same functions, but taken from
   the host's libc (declarations rather than pulling in the system
   header, so as not to fight the order files are looked for in) */
#ifndef STUB_STRING_H
#define STUB_STRING_H

#include <stddef.h>

void *memset(void *dst, int value, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);

#endif
