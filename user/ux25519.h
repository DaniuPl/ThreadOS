/* X25519 - wymiana kluczy na krzywej Curve25519 (RFC 7748)
 *
 * TLS 1.3 zaczyna sie od tego, ze obie strony wymieniaja klucze
 * publiczne i niezaleznie licza ten sam sekret. X25519 jest tu
 * najwygodniejszy: klucz to 32 bajty, nie ma kodowania punktow ani
 * sprawdzania, czy punkt lezy na krzywej.
 *
 * Arytmetyka w ciele 2^255-19 na 16 czlonach po 16 bitow - takie
 * czlony mnozy sie bez przepelnienia w 64 bitach, co na 32-bitowym
 * procesorze bez koprocesora jest najprostsza droga.
 */

#ifndef PROTONOS_UX25519_H
#define PROTONOS_UX25519_H

#include "usys.h"

typedef int64_t gf[16];

static const gf gf_121665 = { 0xDB41, 1 };

/* Przeniesienia miedzy czlonami; 2^256 = 38 (mod 2^255-19) */
static inline void x_carry(gf o)
{
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

/* Zamiana warunkowa bez rozgalezien (b = 0 albo 1) */
static inline void x_swap(gf p, gf q, int b)
{
    int64_t mask = ~((int64_t)b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = mask & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static inline void x_add(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; i++)
        o[i] = a[i] + b[i];
}

static inline void x_sub(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; i++)
        o[i] = a[i] - b[i];
}

static inline void x_mul(gf o, const gf a, const gf b)
{
    int64_t t[31];

    for (int i = 0; i < 31; i++)
        t[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++)         /* zwiniecie gornej polowy */
        t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++)
        o[i] = t[i];
    x_carry(o);
    x_carry(o);
}

static inline void x_sq(gf o, const gf a)
{
    x_mul(o, a, a);
}

/* Odwrotnosc przez podniesienie do potegi p-2 */
static inline void x_inv(gf o, const gf i)
{
    gf c;
    for (int a = 0; a < 16; a++)
        c[a] = i[a];
    for (int a = 253; a >= 0; a--) {
        x_sq(c, c);
        if (a != 2 && a != 4)
            x_mul(c, c, i);
    }
    for (int a = 0; a < 16; a++)
        o[a] = c[a];
}

static inline void x_unpack(gf o, const uint8_t *n)
{
    for (int i = 0; i < 16; i++)
        o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;                     /* najwyzszy bit sie ignoruje */
}

static inline void x_pack(uint8_t *o, const gf n)
{
    gf m, t;

    for (int i = 0; i < 16; i++)
        t[i] = n[i];
    x_carry(t);
    x_carry(t);
    x_carry(t);

    for (int j = 0; j < 2; j++) {        /* pelna redukcja modulo p */
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        x_swap(t, m, 1 - b);
    }

    for (int i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

/* Drabina Montgomery'ego: q = skalar * punkt (oba po 32 bajty) */
static inline void x25519(uint8_t *q, const uint8_t *scalar,
                          const uint8_t *point)
{
    uint8_t z[32];
    gf x, a, b, c, d, e, f;

    for (int i = 0; i < 31; i++)
        z[i] = scalar[i];
    z[31] = (uint8_t)((scalar[31] & 127) | 64);   /* przyciecie skalara */
    z[0] &= 248;

    x_unpack(x, point);
    for (int i = 0; i < 16; i++) {
        b[i] = x[i];
        a[i] = c[i] = d[i] = 0;
    }
    a[0] = d[0] = 1;

    for (int i = 254; i >= 0; i--) {
        int64_t r = (z[i >> 3] >> (i & 7)) & 1;
        x_swap(a, b, (int)r);
        x_swap(c, d, (int)r);
        x_add(e, a, c);
        x_sub(a, a, c);
        x_add(c, b, d);
        x_sub(b, b, d);
        x_sq(d, e);
        x_sq(f, a);
        x_mul(a, c, a);
        x_mul(c, b, e);
        x_add(e, a, c);
        x_sub(a, a, c);
        x_sq(b, a);
        x_sub(c, d, f);
        x_mul(a, c, gf_121665);
        x_add(a, a, d);
        x_mul(c, c, a);
        x_mul(a, d, f);
        x_mul(d, b, x);
        x_sq(b, e);
        x_swap(a, b, (int)r);
        x_swap(c, d, (int)r);
    }

    x_inv(c, c);
    x_mul(a, a, c);
    x_pack(q, a);
}

/* Klucz publiczny z prywatnego: mnozenie przez punkt bazowy u = 9 */
static inline void x25519_base(uint8_t *pub, const uint8_t *priv)
{
    uint8_t base[32];
    for (int i = 0; i < 32; i++)
        base[i] = 0;
    base[0] = 9;
    x25519(pub, priv, base);
}

#endif
