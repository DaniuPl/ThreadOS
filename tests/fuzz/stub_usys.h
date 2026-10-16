/* A stub for the little syscall library, for testing the parsers on
   the host. The parsers do not call the kernel; they need only the
   types and a couple of odds and ends. */

#ifndef PROTONOS_STUB_USYS_H
#define PROTONOS_STUB_USYS_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static inline void print(const char *s) { fputs(s, stdout); }
static inline void print_dec(uint32_t v) { printf("%u", v); }

#endif
