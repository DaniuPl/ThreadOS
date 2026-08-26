/* Zaslepka biblioteczki syscalli do testow parserow na hoscie.
   Parsery nie wolaja jadra, potrzebuja tylko typow i drobiazgow. */

#ifndef PROTONOS_STUB_USYS_H
#define PROTONOS_STUB_USYS_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static inline void print(const char *s) { fputs(s, stdout); }
static inline void print_dec(uint32_t v) { printf("%u", v); }

#endif
