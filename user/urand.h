/* A source of randomness for ring 3 programs
 *
 * The bytes are the kernel's (SYS64_GETRANDOM, kernel/random.c): a
 * generator a process cannot have a copy of. One kept here, seeded
 * once, would hand a forked child the very bytes its parent goes on
 * to use - the same TLS keys on two connections.
 *
 * What stays in the process is a little bookkeeping: the dynamic
 * library test (user/dynlib.c, through user/uthread.h) uses it to see
 * that each process gets the library's data to itself.
 */

#ifndef THREADOS_URAND_H
#define THREADOS_URAND_H

#include "uapi.h"
#include "usha256.h"

static uint8_t urand_seed[32];   /* the first bytes this process drew */
static uint32_t urand_counter;   /* how many 32-byte blocks it has drawn */
static int urand_ready;

static inline void urand_fill(uint8_t *out, uint32_t len)
{
    while (len) {
        uint32_t take = len > 4096 ? 4096 : len;
        if (sys_getrandom(out, take) != (int64_t)take) {
            /* A buffer the kernel refuses is the program's own bug; a
               key made of whatever was there would be worse than none */
            print("[urand] the kernel gave no random bytes\n");
            sys_exit(1);
        }
        out += take;
        len -= take;
    }
}

static inline void urand_init(void)
{
    urand_fill(urand_seed, sizeof(urand_seed));
    urand_counter = 0;
    urand_ready = 1;
}

static inline void urand_bytes(uint8_t *out, uint32_t len)
{
    if (!urand_ready)
        urand_init();
    urand_fill(out, len);
    urand_counter += (len + 31) / 32;
}

#endif
