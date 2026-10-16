#ifndef THREADOS_HEAP_H
#define THREADOS_HEAP_H

#include <stdint.h>
#include <stddef.h>

void heap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);

uint32_t heap_total_bytes(void);
uint32_t heap_used_bytes(void);

#endif
