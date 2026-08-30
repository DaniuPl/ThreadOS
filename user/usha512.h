/* SHA-512 i SHA-384 (FIPS 180-4)
 *
 * Certyfikaty na krzywej P-384 podpisywane sa skrotem SHA-384, wiec bez
 * tego nie da sie ich sprawdzic. Rodzina SHA-512 liczy na slowach
 * 64-bitowych - na 32-bitowym procesorze kompilator rozbija je na pary
 * rejestrow, ale kod pozostaje ten sam.
 *
 * Stale (K oraz wektory poczatkowe) to czesci ulamkowe pierwiastkow
 * kolejnych liczb pierwszych - wygenerowane, nie przepisane recznie.
 */

#ifndef PROTONOS_USHA512_H
#define PROTONOS_USHA512_H

#include "uapi.h"

#define SHA512_SIZE  64
#define SHA384_SIZE  48
#define SHA512_BLOCK 128

static const uint64_t sha512_k[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull,
    0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull,
    0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull,
    0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull,
    0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull,
    0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull,
    0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull,
    0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
    0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull,
    0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull,
    0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull,
    0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull,
    0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull,
    0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull,
    0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull,
    0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull,
    0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull,
    0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull,
    0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull,
    0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull,
    0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

static const uint64_t sha512_h0[8] = {
    0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull,
    0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
    0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
    0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
};

static const uint64_t sha384_h0[8] = {
    0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull,
    0x9159015a3070dd17ull, 0x152fecd8f70e5939ull,
    0x67332667ffc00b31ull, 0x8eb44a8768581511ull,
    0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull,
};

struct sha512 {
    uint64_t state[8];
    uint64_t len_lo, len_hi;      /* dlugosc w bitach (128 bitow w dwoch) */
    uint8_t buf[SHA512_BLOCK];
    uint32_t buflen;
    uint32_t outlen;              /* 64 dla SHA-512, 48 dla SHA-384 */
};

static inline uint64_t s512_ror(uint64_t x, int n)
{
    return (x >> n) | (x << (64 - n));
}

static inline uint64_t s512_get(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static inline void sha512_block(struct sha512 *c, const uint8_t *p)
{
    uint64_t w[80];

    for (int i = 0; i < 16; i++)
        w[i] = s512_get(p + i * 8);
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = s512_ror(w[i - 15], 1) ^ s512_ror(w[i - 15], 8) ^
                      (w[i - 15] >> 7);
        uint64_t s1 = s512_ror(w[i - 2], 19) ^ s512_ror(w[i - 2], 61) ^
                      (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint64_t a = c->state[0], b = c->state[1], cc = c->state[2],
             d = c->state[3], e = c->state[4], f = c->state[5],
             g = c->state[6], h = c->state[7];

    for (int i = 0; i < 80; i++) {
        uint64_t S1 = s512_ror(e, 14) ^ s512_ror(e, 18) ^ s512_ror(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + sha512_k[i] + w[i];
        uint64_t S0 = s512_ror(a, 28) ^ s512_ror(a, 34) ^ s512_ror(a, 39);
        uint64_t may_flag = (a & b) ^ (a & cc) ^ (b & cc);
        uint64_t t2 = S0 + may_flag;

        h = g; g = f; f = e;
        e = d + t1;
        d = cc; cc = b; b = a;
        a = t1 + t2;
    }

    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static inline void sha512_init_with(struct sha512 *c, const uint64_t *h0,
                                    uint32_t outlen)
{
    for (int i = 0; i < 8; i++)
        c->state[i] = h0[i];
    c->len_lo = c->len_hi = 0;
    c->buflen = 0;
    c->outlen = outlen;
}

static inline void sha512_init(struct sha512 *c)
{
    sha512_init_with(c, sha512_h0, SHA512_SIZE);
}

static inline void sha384_init(struct sha512 *c)
{
    sha512_init_with(c, sha384_h0, SHA384_SIZE);
}

static inline void sha512_update(struct sha512 *c, const void *data,
                                 uint32_t len)
{
    const uint8_t *p = data;

    uint64_t bits = (uint64_t)len << 3;
    uint64_t old = c->len_lo;
    c->len_lo += bits;
    if (c->len_lo < old)
        c->len_hi++;

    while (len) {
        uint32_t take = SHA512_BLOCK - c->buflen;
        if (take > len)
            take = len;
        for (uint32_t i = 0; i < take; i++)
            c->buf[c->buflen + i] = p[i];
        c->buflen += take;
        p += take;
        len -= take;
        if (c->buflen == SHA512_BLOCK) {
            sha512_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

static inline void sha512_final(struct sha512 *c, uint8_t *out)
{
    uint64_t lo = c->len_lo, hi = c->len_hi;

    uint8_t pad = 0x80;
    sha512_update(c, &pad, 1);
    c->len_lo = lo; c->len_hi = hi;      /* dopelnienie nie liczy sie */

    uint8_t zero = 0;
    while (c->buflen != 112) {           /* 128 - 16 na dlugosc */
        sha512_update(c, &zero, 1);
        c->len_lo = lo; c->len_hi = hi;
    }

    uint8_t lenbuf[16];
    for (int i = 0; i < 8; i++)
        lenbuf[i] = (uint8_t)(hi >> (56 - 8 * i));
    for (int i = 0; i < 8; i++)
        lenbuf[8 + i] = (uint8_t)(lo >> (56 - 8 * i));
    sha512_update(c, lenbuf, 16);

    for (uint32_t i = 0; i < c->outlen; i++)
        out[i] = (uint8_t)(c->state[i / 8] >> (56 - 8 * (i % 8)));
}

static inline void sha384(const void *data, uint32_t len, uint8_t *out)
{
    struct sha512 c;
    sha384_init(&c);
    sha512_update(&c, data, len);
    sha512_final(&c, out);
}

static inline void sha512(const void *data, uint32_t len, uint8_t *out)
{
    struct sha512 c;
    sha512_init(&c);
    sha512_update(&c, data, len);
    sha512_final(&c, out);
}

#endif
