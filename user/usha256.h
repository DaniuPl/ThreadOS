/* SHA-256, HMAC-SHA256 i HKDF (RFC 6234, 2104, 5869)
 *
 * Pierwsza cegielka pod TLS 1.3: cala jego kryptografia stoi na funkcji
 * skrotu i wyprowadzaniu kluczy. Kod jest 32-bitowy i bez zaleznosci -
 * programy ring 3 nie maja libc.
 */

#ifndef PROTONOS_USHA256_H
#define PROTONOS_USHA256_H

#include "uapi.h"

#define SHA256_SIZE  32
#define SHA256_BLOCK 64

struct sha256 {
    uint32_t state[8];
    uint32_t len_lo, len_hi;      /* dlugosc w bitach (64 bity w dwoch) */
    uint8_t buf[SHA256_BLOCK];
    uint32_t buflen;
};

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline uint32_t sha256_ror(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static inline void sha256_block(struct sha256 *c, const uint8_t *p)
{
    uint32_t w[64];

    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = sha256_ror(w[i - 15], 7) ^ sha256_ror(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = sha256_ror(w[i - 2], 17) ^ sha256_ror(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2],
             d = c->state[3], e = c->state[4], f = c->state[5],
             g = c->state[6], h = c->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = sha256_ror(e, 6) ^ sha256_ror(e, 11) ^ sha256_ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = sha256_ror(a, 2) ^ sha256_ror(a, 13) ^ sha256_ror(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;

        h = g; g = f; f = e;
        e = d + t1;
        d = cc; cc = b; b = a;
        a = t1 + t2;
    }

    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static inline void sha256_init(struct sha256 *c)
{
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->len_lo = c->len_hi = 0;
    c->buflen = 0;
}

static inline void sha256_update(struct sha256 *c, const void *data,
                                 uint32_t len)
{
    const uint8_t *p = data;

    uint32_t bits = len << 3;
    uint32_t old = c->len_lo;
    c->len_lo += bits;
    if (c->len_lo < old)
        c->len_hi++;
    c->len_hi += len >> 29;

    while (len) {
        uint32_t take = SHA256_BLOCK - c->buflen;
        if (take > len)
            take = len;
        for (uint32_t i = 0; i < take; i++)
            c->buf[c->buflen + i] = p[i];
        c->buflen += take;
        p += take;
        len -= take;
        if (c->buflen == SHA256_BLOCK) {
            sha256_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

static inline void sha256_final(struct sha256 *c, uint8_t *out)
{
    uint32_t lo = c->len_lo, hi = c->len_hi;

    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    c->len_lo = lo; c->len_hi = hi;        /* dopelnienie nie liczy sie */

    uint8_t zero = 0;
    while (c->buflen != 56) {
        sha256_update(c, &zero, 1);
        c->len_lo = lo; c->len_hi = hi;
    }

    uint8_t lenbuf[8];
    lenbuf[0] = (uint8_t)(hi >> 24); lenbuf[1] = (uint8_t)(hi >> 16);
    lenbuf[2] = (uint8_t)(hi >> 8);  lenbuf[3] = (uint8_t)hi;
    lenbuf[4] = (uint8_t)(lo >> 24); lenbuf[5] = (uint8_t)(lo >> 16);
    lenbuf[6] = (uint8_t)(lo >> 8);  lenbuf[7] = (uint8_t)lo;
    sha256_update(c, lenbuf, 8);

    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(c->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c->state[i];
    }
}

static inline void sha256(const void *data, uint32_t len, uint8_t *out)
{
    struct sha256 c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

/* HMAC-SHA256 (RFC 2104) */
static inline void hmac_sha256(const uint8_t *key, uint32_t keylen,
                               const void *data, uint32_t len, uint8_t *out)
{
    uint8_t k[SHA256_BLOCK];
    uint8_t inner[SHA256_SIZE];
    struct sha256 c;

    for (uint32_t i = 0; i < SHA256_BLOCK; i++)
        k[i] = 0;
    if (keylen > SHA256_BLOCK) {
        sha256(key, keylen, k);
    } else {
        for (uint32_t i = 0; i < keylen; i++)
            k[i] = key[i];
    }

    uint8_t pad[SHA256_BLOCK];
    for (int i = 0; i < SHA256_BLOCK; i++)
        pad[i] = (uint8_t)(k[i] ^ 0x36);
    sha256_init(&c);
    sha256_update(&c, pad, SHA256_BLOCK);
    sha256_update(&c, data, len);
    sha256_final(&c, inner);

    for (int i = 0; i < SHA256_BLOCK; i++)
        pad[i] = (uint8_t)(k[i] ^ 0x5c);
    sha256_init(&c);
    sha256_update(&c, pad, SHA256_BLOCK);
    sha256_update(&c, inner, SHA256_SIZE);
    sha256_final(&c, out);
}

/* HKDF (RFC 5869): wyciagniecie i rozwiniecie */
static inline void hkdf_extract(const uint8_t *salt, uint32_t saltlen,
                                const uint8_t *ikm, uint32_t ikmlen,
                                uint8_t *prk)
{
    uint8_t zero[SHA256_SIZE];
    if (!salt || !saltlen) {
        for (int i = 0; i < SHA256_SIZE; i++)
            zero[i] = 0;
        salt = zero;
        saltlen = SHA256_SIZE;
    }
    hmac_sha256(salt, saltlen, ikm, ikmlen, prk);
}

static inline void hkdf_expand(const uint8_t *prk, const uint8_t *info,
                               uint32_t infolen, uint8_t *out, uint32_t outlen)
{
    uint8_t t[SHA256_SIZE];
    uint8_t block[SHA256_SIZE + 256 + 1];
    uint32_t tlen = 0, done = 0;
    uint8_t counter = 1;

    while (done < outlen) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < tlen; i++)
            block[n++] = t[i];
        for (uint32_t i = 0; i < infolen && n < sizeof(block) - 1; i++)
            block[n++] = info[i];
        block[n++] = counter++;

        hmac_sha256(prk, SHA256_SIZE, block, n, t);
        tlen = SHA256_SIZE;

        uint32_t take = outlen - done;
        if (take > SHA256_SIZE)
            take = SHA256_SIZE;
        for (uint32_t i = 0; i < take; i++)
            out[done + i] = t[i];
        done += take;
    }
}

/* HKDF-Expand-Label z TLS 1.3 (RFC 8446 7.1) */
static inline void hkdf_expand_label(const uint8_t *secret, const char *label,
                                     const uint8_t *ctx, uint32_t ctxlen,
                                     uint8_t *out, uint32_t outlen)
{
    uint8_t info[512];
    uint32_t n = 0;

    info[n++] = (uint8_t)(outlen >> 8);
    info[n++] = (uint8_t)outlen;

    uint32_t llen = 0;
    while (label[llen])
        llen++;
    info[n++] = (uint8_t)(6 + llen);          /* "tls13 " + etykieta */
    const char *pre = "tls13 ";
    for (int i = 0; i < 6; i++)
        info[n++] = (uint8_t)pre[i];
    for (uint32_t i = 0; i < llen; i++)
        info[n++] = (uint8_t)label[i];

    info[n++] = (uint8_t)ctxlen;
    for (uint32_t i = 0; i < ctxlen && n < sizeof(info); i++)
        info[n++] = ctx[i];

    hkdf_expand(secret, info, n, out, outlen);
}

#endif
