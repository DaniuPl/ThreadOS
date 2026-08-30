#ifndef PROTONOS_PIT_H
#define PROTONOS_PIT_H

#include <stdint.h>

/* Programs the PIT to a given frequency and unmasks IRQ0 */
void pit_init(uint32_t frequency_hz);

/* Number of ticks since startup (at 100 Hz: 1 tick = 10 ms) */
uint32_t pit_ticks(void);

#endif
