/* Stand-in for the kernel's heap.h in the host harness: the C
 * library's allocator, which the address sanitizer watches */
#ifndef KSTUB_HEAP_H
#define KSTUB_HEAP_H

#include <stdlib.h>

static inline void *kmalloc(size_t n) { return malloc(n); }
static inline void kfree(void *p) { free(p); }

#endif
