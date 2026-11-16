#ifndef THREADOS_RANDOM_H
#define THREADOS_RANDOM_H

#include <stdint.h>

/* Random bytes fit for keys (kernel/random.c) */

/* Checks the primitives and seeds the pool: after the timer runs */
void random_init(void);

/* Called on every interrupt: when it came, in processor cycles */
void random_add_interrupt(int irq);

/* Bytes that may or may not be secret: they can only add to the pool */
void random_add(const void *data, uint32_t n);

/* 1 once the pool holds enough that its output can be trusted */
int random_ready(void);

void random_bytes(void *out, uint32_t n);

#endif
