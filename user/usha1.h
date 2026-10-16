/* SHA-1, HMAC-SHA1 and PBKDF2 - only where a standard insists
 *
 * SHA-1 is broken for signatures and has no business anywhere near
 * a certificate; this system verifies certificates with SHA-256 and
 * SHA-512 and that is not going to change.
 *
 * It is here for one reason: WPA2. The password of a wireless network
 * turns into a key through PBKDF2-HMAC-SHA1 with four thousand and
 * ninety-six iterations (IEEE 802.11i), and the four-way handshake
 * derives the session keys with a function built out of HMAC-SHA1.
 * Neither is negotiable - a network will not talk to us in any other
 * dialect - and neither is endangered by the collisions that finished
 * SHA-1 off, because both use it as a key derivation function rather
 * than as a fingerprint of a document somebody signs.
 */

#ifndef THREADOS_USHA1_H
#define THREADOS_USHA1_H

#include "uapi.h"

#define SHA1_SIZE  20
#define SHA1_BLOCK 64

struct sha1 {
    uint32_t state[5];
    uint32_t len_lo, len_hi;      /* the length in bits */
    uint8_t buf[SHA1_BLOCK];
    uint32_t buflen;
};

static inline uint32_t sha1_rol(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static inline void sha1_block(struct sha1 *c, const uint8_t *p)
{
    uint32_t w[80];

    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    /* The expansion is where SHA-1 differs from SHA-0 - by this one
       rotation, and that one rotation is the whole difference between a
       function broken in a day and one that lasted fifteen years. */
    for (int i = 16; i < 80; i++)
        w[i] = sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = c->state[0], b = c->state[1], d = c->state[2],
             e = c->state[3], f = c->state[4];

    for (int i = 0; i < 80; i++) {
        uint32_t mix, k;

        if (i < 20) {
            mix = (b & d) | (~b & e);
            k = 0x5a827999u;
        } else if (i < 40) {
            mix = b ^ d ^ e;
            k = 0x6ed9eba1u;
        } else if (i < 60) {
            mix = (b & d) | (b & e) | (d & e);
            k = 0x8f1bbcdcu;
        } else {
            mix = b ^ d ^ e;
            k = 0xca62c1d6u;
        }

        uint32_t t = sha1_rol(a, 5) + mix + f + k + w[i];
        f = e;
        e = d;
        d = sha1_rol(b, 30);
        b = a;
        a = t;
    }

    c->state[0] += a; c->state[1] += b; c->state[2] += d;
    c->state[3] += e; c->state[4] += f;
}

static inline void sha1_init(struct sha1 *c)
{
    c->state[0] = 0x67452301u;
    c->state[1] = 0xefcdab89u;
    c->state[2] = 0x98badcfeu;
    c->state[3] = 0x10325476u;
    c->state[4] = 0xc3d2e1f0u;
    c->len_lo = c->len_hi = 0;
    c->buflen = 0;
}

static inline void sha1_update(struct sha1 *c, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t bits = len << 3;

    c->len_lo += bits;
    if (c->len_lo < bits)
        c->len_hi++;
    c->len_hi += len >> 29;

    while (len) {
        uint32_t n = SHA1_BLOCK - c->buflen;
        if (n > len)
            n = len;
        for (uint32_t i = 0; i < n; i++)
            c->buf[c->buflen + i] = p[i];
        c->buflen += n;
        p += n;
        len -= n;
        if (c->buflen == SHA1_BLOCK) {
            sha1_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

static inline void sha1_final(struct sha1 *c, uint8_t out[SHA1_SIZE])
{
    uint32_t lo = c->len_lo, hi = c->len_hi;
    uint8_t pad = 0x80;

    sha1_update(c, &pad, 1);
    pad = 0;
    while (c->buflen != 56)
        sha1_update(c, &pad, 1);

    uint8_t lens[8];
    for (int i = 0; i < 4; i++) {
        lens[i] = (uint8_t)(hi >> (24 - 8 * i));
        lens[4 + i] = (uint8_t)(lo >> (24 - 8 * i));
    }
    sha1_update(c, lens, 8);

    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(c->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c->state[i];
    }
}

static inline void sha1(const void *data, uint32_t len, uint8_t out[SHA1_SIZE])
{
    struct sha1 c;

    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, out);
}

/* HMAC-SHA1 (RFC 2104) */
static inline void hmac_sha1(const uint8_t *key, uint32_t keylen,
                             const void *data, uint32_t len,
                             uint8_t out[SHA1_SIZE])
{
    uint8_t k[SHA1_BLOCK];
    uint8_t pad[SHA1_BLOCK];
    uint8_t inner[SHA1_SIZE];
    struct sha1 c;

    for (uint32_t i = 0; i < SHA1_BLOCK; i++)
        k[i] = 0;
    if (keylen > SHA1_BLOCK)
        sha1(key, keylen, k);
    else
        for (uint32_t i = 0; i < keylen; i++)
            k[i] = key[i];

    for (int i = 0; i < SHA1_BLOCK; i++)
        pad[i] = (uint8_t)(k[i] ^ 0x36);
    sha1_init(&c);
    sha1_update(&c, pad, SHA1_BLOCK);
    sha1_update(&c, data, len);
    sha1_final(&c, inner);

    for (int i = 0; i < SHA1_BLOCK; i++)
        pad[i] = (uint8_t)(k[i] ^ 0x5c);
    sha1_init(&c);
    sha1_update(&c, pad, SHA1_BLOCK);
    sha1_update(&c, inner, SHA1_SIZE);
    sha1_final(&c, out);
}

/* PBKDF2-HMAC-SHA1 (RFC 2898)
 *
 * The whole point of it is to be SLOW: a password is short, so the only
 * thing standing between it and a dictionary is how much work one guess
 * costs. WPA2 asks for four thousand and ninety-six iterations over
 * every block - about eight thousand SHA-1 compressions for a key, and
 * that is deliberate.
 */
static inline void pbkdf2_sha1(const uint8_t *pass, uint32_t passlen,
                               const uint8_t *salt, uint32_t saltlen,
                               uint32_t rounds, uint8_t *out, uint32_t outlen)
{
    uint8_t block[128 + 4];
    uint8_t u[SHA1_SIZE], acc[SHA1_SIZE];
    uint32_t counter = 1;

    if (saltlen > 128)
        saltlen = 128;

    while (outlen) {
        uint32_t n = outlen < SHA1_SIZE ? outlen : SHA1_SIZE;

        for (uint32_t i = 0; i < saltlen; i++)
            block[i] = salt[i];
        /* The counter goes after the salt, four bytes, big-endian -
           this is what makes every block of the result different. */
        block[saltlen] = (uint8_t)(counter >> 24);
        block[saltlen + 1] = (uint8_t)(counter >> 16);
        block[saltlen + 2] = (uint8_t)(counter >> 8);
        block[saltlen + 3] = (uint8_t)counter;

        hmac_sha1(pass, passlen, block, saltlen + 4, u);
        for (uint32_t i = 0; i < SHA1_SIZE; i++)
            acc[i] = u[i];

        for (uint32_t r = 1; r < rounds; r++) {
            hmac_sha1(pass, passlen, u, SHA1_SIZE, u);
            for (uint32_t i = 0; i < SHA1_SIZE; i++)
                acc[i] ^= u[i];
        }

        for (uint32_t i = 0; i < n; i++)
            out[i] = acc[i];
        out += n;
        outlen -= n;
        counter++;
    }
}

#endif
