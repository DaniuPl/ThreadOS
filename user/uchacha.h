/* ChaCha20 i Poly1305 (RFC 8439) - szyfr AEAD dla TLS 1.3
 *
 * Wybrany zamiast AES-GCM, bo caly liczy sie na 32-bitowych dodawaniach,
 * obrotach i XOR-ach: bez tablic podstawien, bez mnozen w ciele
 * skonczonym i bez ryzyka, ze zabraknie nam pamieci na tablice.
 * Serwery TLS 1.3 powszechnie go akceptuja.
 */

#ifndef PROTONOS_UCHACHA_H
#define PROTONOS_UCHACHA_H

#include "uapi.h"

#define CHACHA_KEY   32
#define CHACHA_NONCE 12
#define POLY1305_TAG 16

static inline uint32_t cc_rotl(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static inline uint32_t cc_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void cc_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

#define CC_QR(a, b, c, d) do {                        \
        a += b; d ^= a; d = cc_rotl(d, 16);           \
        c += d; b ^= c; b = cc_rotl(b, 12);           \
        a += b; d ^= a; d = cc_rotl(d, 8);            \
        c += d; b ^= c; b = cc_rotl(b, 7);            \
    } while (0)

/* Jeden blok strumienia klucza (64 B) dla licznika 'counter' */
static inline void chacha20_block(const uint8_t key[32], uint32_t counter,
                                  const uint8_t nonce[12], uint8_t out[64])
{
    static const char sigma[] = "expand 32-byte k";
    uint32_t s[16], x[16];

    for (int i = 0; i < 4; i++)
        s[i] = cc_le32((const uint8_t *)sigma + i * 4);
    for (int i = 0; i < 8; i++)
        s[4 + i] = cc_le32(key + i * 4);
    s[12] = counter;
    for (int i = 0; i < 3; i++)
        s[13 + i] = cc_le32(nonce + i * 4);

    for (int i = 0; i < 16; i++)
        x[i] = s[i];

    for (int r = 0; r < 10; r++) {          /* 20 rund = 10 podwojnych */
        CC_QR(x[0], x[4], x[8],  x[12]);
        CC_QR(x[1], x[5], x[9],  x[13]);
        CC_QR(x[2], x[6], x[10], x[14]);
        CC_QR(x[3], x[7], x[11], x[15]);
        CC_QR(x[0], x[5], x[10], x[15]);
        CC_QR(x[1], x[6], x[11], x[12]);
        CC_QR(x[2], x[7], x[8],  x[13]);
        CC_QR(x[3], x[4], x[9],  x[14]);
    }

    for (int i = 0; i < 16; i++)
        cc_put32(out + i * 4, x[i] + s[i]);
}

/* Szyfrowanie strumieniowe (deszyfrowanie to ta sama operacja) */
static inline void chacha20_xor(const uint8_t key[32], uint32_t counter,
                                const uint8_t nonce[12], const uint8_t *in,
                                uint8_t *out, uint32_t len)
{
    uint8_t block[64];
    uint32_t done = 0;

    while (done < len) {
        chacha20_block(key, counter++, nonce, block);
        uint32_t take = len - done;
        if (take > 64)
            take = 64;
        for (uint32_t i = 0; i < take; i++)
            out[done + i] = (uint8_t)(in[done + i] ^ block[i]);
        done += take;
    }
}

/* --- Poly1305 -------------------------------------------------------- */

struct poly1305 {
    uint32_t r[5], h[5], pad[4];
    uint8_t buf[16];
    uint32_t buflen;
};

static inline void poly1305_blocks(struct poly1305 *st, const uint8_t *m,
                                   uint32_t bytes, int final)
{
    const uint32_t hibit = final ? 0 : (1u << 24);

    while (bytes >= 16) {
        uint32_t t0 = cc_le32(m + 0), t1 = cc_le32(m + 4);
        uint32_t t2 = cc_le32(m + 8), t3 = cc_le32(m + 12);

        st->h[0] += t0 & 0x3ffffff;
        st->h[1] += ((t0 >> 26) | (t1 << 6)) & 0x3ffffff;
        st->h[2] += ((t1 >> 20) | (t2 << 12)) & 0x3ffffff;
        st->h[3] += ((t2 >> 14) | (t3 << 18)) & 0x3ffffff;
        st->h[4] += (t3 >> 8) | hibit;

        uint64_t d0 = (uint64_t)st->h[0] * st->r[0] +
                      (uint64_t)st->h[1] * (5 * st->r[4]) +
                      (uint64_t)st->h[2] * (5 * st->r[3]) +
                      (uint64_t)st->h[3] * (5 * st->r[2]) +
                      (uint64_t)st->h[4] * (5 * st->r[1]);
        uint64_t d1 = (uint64_t)st->h[0] * st->r[1] +
                      (uint64_t)st->h[1] * st->r[0] +
                      (uint64_t)st->h[2] * (5 * st->r[4]) +
                      (uint64_t)st->h[3] * (5 * st->r[3]) +
                      (uint64_t)st->h[4] * (5 * st->r[2]);
        uint64_t d2 = (uint64_t)st->h[0] * st->r[2] +
                      (uint64_t)st->h[1] * st->r[1] +
                      (uint64_t)st->h[2] * st->r[0] +
                      (uint64_t)st->h[3] * (5 * st->r[4]) +
                      (uint64_t)st->h[4] * (5 * st->r[3]);
        uint64_t d3 = (uint64_t)st->h[0] * st->r[3] +
                      (uint64_t)st->h[1] * st->r[2] +
                      (uint64_t)st->h[2] * st->r[1] +
                      (uint64_t)st->h[3] * st->r[0] +
                      (uint64_t)st->h[4] * (5 * st->r[4]);
        uint64_t d4 = (uint64_t)st->h[0] * st->r[4] +
                      (uint64_t)st->h[1] * st->r[3] +
                      (uint64_t)st->h[2] * st->r[2] +
                      (uint64_t)st->h[3] * st->r[1] +
                      (uint64_t)st->h[4] * st->r[0];

        uint32_t c;
        c = (uint32_t)(d0 >> 26); st->h[0] = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); st->h[1] = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); st->h[2] = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); st->h[3] = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); st->h[4] = (uint32_t)d4 & 0x3ffffff;
        st->h[0] += c * 5; c = st->h[0] >> 26; st->h[0] &= 0x3ffffff;
        st->h[1] += c;

        m += 16;
        bytes -= 16;
    }
}

static inline void poly1305_init(struct poly1305 *st, const uint8_t key[32])
{
    uint32_t t0 = cc_le32(key + 0), t1 = cc_le32(key + 4);
    uint32_t t2 = cc_le32(key + 8), t3 = cc_le32(key + 12);

    /* r jest "przycinane" zgodnie z RFC 8439 */
    st->r[0] = t0 & 0x3ffffff;
    st->r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03;
    st->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ff;
    st->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fff;
    st->r[4] = (t3 >> 8) & 0x00fffff;

    for (int i = 0; i < 5; i++)
        st->h[i] = 0;
    for (int i = 0; i < 4; i++)
        st->pad[i] = cc_le32(key + 16 + i * 4);
    st->buflen = 0;
}

static inline void poly1305_update(struct poly1305 *st, const uint8_t *m,
                                   uint32_t bytes)
{
    if (st->buflen) {
        uint32_t take = 16 - st->buflen;
        if (take > bytes)
            take = bytes;
        for (uint32_t i = 0; i < take; i++)
            st->buf[st->buflen + i] = m[i];
        st->buflen += take;
        m += take;
        bytes -= take;
        if (st->buflen == 16) {
            poly1305_blocks(st, st->buf, 16, 0);
            st->buflen = 0;
        }
    }
    if (bytes >= 16) {
        uint32_t want = bytes & ~15u;
        poly1305_blocks(st, m, want, 0);
        m += want;
        bytes -= want;
    }
    for (uint32_t i = 0; i < bytes; i++)
        st->buf[st->buflen++] = m[i];
}

static inline void poly1305_final(struct poly1305 *st, uint8_t tag[16])
{
    if (st->buflen) {
        st->buf[st->buflen++] = 1;
        while (st->buflen < 16)
            st->buf[st->buflen++] = 0;
        poly1305_blocks(st, st->buf, 16, 1);
    }

    uint32_t c;
    c = st->h[1] >> 26; st->h[1] &= 0x3ffffff;
    st->h[2] += c; c = st->h[2] >> 26; st->h[2] &= 0x3ffffff;
    st->h[3] += c; c = st->h[3] >> 26; st->h[3] &= 0x3ffffff;
    st->h[4] += c; c = st->h[4] >> 26; st->h[4] &= 0x3ffffff;
    st->h[0] += c * 5; c = st->h[0] >> 26; st->h[0] &= 0x3ffffff;
    st->h[1] += c;

    /* h - p (p = 2^130 - 5) i wybor mniejszej wartosci */
    uint32_t g[5];
    g[0] = st->h[0] + 5; c = g[0] >> 26; g[0] &= 0x3ffffff;
    g[1] = st->h[1] + c; c = g[1] >> 26; g[1] &= 0x3ffffff;
    g[2] = st->h[2] + c; c = g[2] >> 26; g[2] &= 0x3ffffff;
    g[3] = st->h[3] + c; c = g[3] >> 26; g[3] &= 0x3ffffff;
    g[4] = st->h[4] + c - (1u << 26);

    uint32_t mask = (g[4] >> 31) - 1;      /* 0 gdy h < p, same jedynki gdy >= */
    for (int i = 0; i < 5; i++)
        g[i] &= mask;
    mask = ~mask;
    for (int i = 0; i < 5; i++)
        st->h[i] = (st->h[i] & mask) | g[i];

    uint32_t f0 = (st->h[0] | (st->h[1] << 26)) & 0xffffffffu;
    uint32_t f1 = ((st->h[1] >> 6) | (st->h[2] << 20)) & 0xffffffffu;
    uint32_t f2 = ((st->h[2] >> 12) | (st->h[3] << 14)) & 0xffffffffu;
    uint32_t f3 = ((st->h[3] >> 18) | (st->h[4] << 8)) & 0xffffffffu;

    uint64_t t = (uint64_t)f0 + st->pad[0];
    f0 = (uint32_t)t;
    t = (uint64_t)f1 + st->pad[1] + (t >> 32);
    f1 = (uint32_t)t;
    t = (uint64_t)f2 + st->pad[2] + (t >> 32);
    f2 = (uint32_t)t;
    t = (uint64_t)f3 + st->pad[3] + (t >> 32);
    f3 = (uint32_t)t;

    cc_put32(tag + 0, f0);
    cc_put32(tag + 4, f1);
    cc_put32(tag + 8, f2);
    cc_put32(tag + 12, f3);
}

static inline void poly1305(const uint8_t key[32], const uint8_t *m,
                            uint32_t bytes, uint8_t tag[16])
{
    struct poly1305 st;
    poly1305_init(&st, key);
    poly1305_update(&st, m, bytes);
    poly1305_final(&st, tag);
}

/* --- AEAD_CHACHA20_POLY1305 (RFC 8439 2.8) ---------------------------- */

static inline void aead_tag(const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t *aad, uint32_t aadlen,
                            const uint8_t *ct, uint32_t ctlen, uint8_t tag[16])
{
    uint8_t polykey[64];
    uint8_t pad[16] = {0};
    uint8_t lens[16];
    struct poly1305 st;

    chacha20_block(key, 0, nonce, polykey);   /* licznik 0 = klucz Poly1305 */
    poly1305_init(&st, polykey);

    poly1305_update(&st, aad, aadlen);
    if (aadlen % 16)
        poly1305_update(&st, pad, 16 - (aadlen % 16));
    poly1305_update(&st, ct, ctlen);
    if (ctlen % 16)
        poly1305_update(&st, pad, 16 - (ctlen % 16));

    for (int i = 0; i < 16; i++)
        lens[i] = 0;
    cc_put32(lens + 0, aadlen);
    cc_put32(lens + 8, ctlen);
    poly1305_update(&st, lens, 16);

    poly1305_final(&st, tag);
}

/* Szyfruje w miejscu i dopisuje 16-bajtowy znacznik do 'tag' */
static inline void aead_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                                const uint8_t *aad, uint32_t aadlen,
                                const uint8_t *pt, uint8_t *ct, uint32_t len,
                                uint8_t tag[16])
{
    chacha20_xor(key, 1, nonce, pt, ct, len);  /* dane od licznika 1 */
    aead_tag(key, nonce, aad, aadlen, ct, len, tag);
}

/* Sprawdza znacznik i odszyfrowuje. 1 = OK, 0 = znacznik sie nie zgadza. */
static inline int aead_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t *aad, uint32_t aadlen,
                               const uint8_t *ct, uint8_t *pt, uint32_t len,
                               const uint8_t tag[16])
{
    uint8_t want[16];
    aead_tag(key, nonce, aad, aadlen, ct, len, want);

    uint8_t diff = 0;
    for (int i = 0; i < 16; i++)
        diff |= (uint8_t)(want[i] ^ tag[i]);
    if (diff)
        return 0;

    chacha20_xor(key, 1, nonce, ct, pt, len);
    return 1;
}

#endif
