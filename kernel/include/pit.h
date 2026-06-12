#ifndef PROTONOS_PIT_H
#define PROTONOS_PIT_H

#include <stdint.h>

/* Programuje PIT na zadaną częstotliwość i odmaskowuje IRQ0 */
void pit_init(uint32_t frequency_hz);

/* Liczba tyknięć od startu (przy 100 Hz: 1 tyknięcie = 10 ms) */
uint32_t pit_ticks(void);

#endif
