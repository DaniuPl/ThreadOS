#ifndef STUB_PIT_H
#define STUB_PIT_H
#include <stdint.h>
static uint32_t stub_ticks;
static inline uint32_t pit_ticks(void) { return ++stub_ticks; }
#endif
