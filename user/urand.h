/* Zrodlo losowosci dla programow ring 3
 *
 * TLS potrzebuje nieprzewidywalnych bajtow (klucz prywatny, losowa
 * czesc ClientHello). ProtonOS nie ma sprzetowego generatora, wiec
 * mieszamy to, czym dysponujemy: zegar czasu rzeczywistego, licznik
 * tyknięc timera, pid procesu i adresy ze stosu. Kolejne bajty
 * wychodza z SHA-256 licznika i ziarna.
 *
 * Uczciwie: to nie jest generator kryptograficzny z prawdziwego
 * zdarzenia - entropii jest tu niewiele. Wystarcza do zabawkowego
 * klienta TLS, ale nie do ochrony czegokolwiek wartosciowego.
 */

#ifndef PROTONOS_URAND_H
#define PROTONOS_URAND_H

#include "uapi.h"
#include "usha256.h"

static uint8_t urand_seed[32];
static uint32_t urand_counter;   /* ile blokow juz wydano - stan procesu */
static int urand_ready;

static inline void urand_init(void)
{
    struct utime t;
    uint8_t mix[64];
    uint32_t n = 0;
    int stack_marker = 0;

    sys_time(&t);
    uint32_t ticks = sys_getticks();
    uint32_t pid = (uint32_t)sys_getpid();
    /* Adres na stosie - w trybie dlugim wskaznik ma osiem bajtow,
       wiec przez uintptr_t, a dopiero potem obciecie do 32 bitow */
    uint32_t addr = (uint32_t)(uintptr_t)&stack_marker;

    mix[n++] = (uint8_t)t.year; mix[n++] = (uint8_t)(t.year >> 8);
    mix[n++] = t.month; mix[n++] = t.day;
    mix[n++] = t.hour;  mix[n++] = t.min;  mix[n++] = t.sec;
    for (int i = 0; i < 4; i++) mix[n++] = (uint8_t)(ticks >> (i * 8));
    for (int i = 0; i < 4; i++) mix[n++] = (uint8_t)(pid >> (i * 8));
    for (int i = 0; i < 4; i++) mix[n++] = (uint8_t)(addr >> (i * 8));

    /* Odczyty zegara w petli lapia jeszcze troche zmiennosci */
    for (int k = 0; k < 8 && n < (uint32_t)sizeof(mix); k++)
        mix[n++] = (uint8_t)sys_getticks();

    sha256(mix, n, urand_seed);
    urand_counter = 0;
    urand_ready = 1;
}

static inline void urand_bytes(uint8_t *out, uint32_t len)
{
    uint8_t block[32 + 4];
    uint8_t hash[32];
    uint32_t done = 0;

    if (!urand_ready)
        urand_init();

    while (done < len) {
        for (int i = 0; i < 32; i++)
            block[i] = urand_seed[i];
        for (int i = 0; i < 4; i++)
            block[32 + i] = (uint8_t)(urand_counter >> (i * 8));
        urand_counter++;

        sha256(block, sizeof(block), hash);
        uint32_t take = len - done;
        if (take > 32)
            take = 32;
        for (uint32_t i = 0; i < take; i++)
            out[done + i] = hash[i];
        done += take;
    }
}

#endif
