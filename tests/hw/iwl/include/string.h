/* What kernel/include/string.h declares, and nothing more: a driver
   using another C library function would build here and not in the
   kernel */
#ifndef HW_IWL_STRING_H
#define HW_IWL_STRING_H
#include <stddef.h>
void *memset(void *dst, int value, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
#endif
