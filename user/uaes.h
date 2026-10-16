/* AES with the two modes that seal a packet: GCM and CCM
 *
 * Until now the whole of ThreadOS' cryptography stood on ChaCha20 -
 * chosen because it computes on additions, rotations and XORs alone.
 * AES is needed for what ChaCha cannot reach:
 *
 *   GCM - the cipher suite that every TLS 1.3 server offers, and many
 *         an embedded one offers ONLY that;
 *   CCM - what WPA2 encrypts a wireless frame with (CCMP is AES-CCM
 *         with L=2 and an eight-byte tag).
 *
 * The implementation keeps one 256-byte substitution table (and its
 * inverse) and computes MixColumns rather than reading it out of four
 * kilobytes of precomputed tables. That is slower, but it is also the
 * only version whose every line can be read against FIPS-197.
 *
 * A word of honesty about what this is NOT: the table lookups make the
 * time depend on the data, so an attacker measuring the cache could
 * learn something about the key. Doing better means either the AES-NI
 * instructions or a bitsliced implementation - neither belongs in the
 * first version.
 */

#ifndef THREADOS_UAES_H
#define THREADOS_UAES_H

#include "uapi.h"

#define AES_BLOCK   16
#define AES_MAXKEYS 240        /* 15 round keys of 16 bytes = AES-256 */

struct aes {
    uint8_t rk[AES_MAXKEYS];
    int rounds;                /* 10, 12 or 14 - as many as the key wants */
};

static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static const uint8_t aes_inv_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d,
};

/* Multiplication by x in GF(2^8) with the polynomial x^8+x^4+x^3+x+1:
   a shift, and when the bit that fell off the top was set, a xor with
   0x1b - that is the reduction, written down. */
static inline uint8_t aes_xtime(uint8_t a)
{
    return (uint8_t)((a << 1) ^ ((a >> 7) * 0x1b));
}

static inline uint8_t aes_mul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        r ^= (uint8_t)(a * (b & 1));
        a = aes_xtime(a);
        b >>= 1;
    }
    return r;
}

/* The key schedule (FIPS-197 5.2). Words of four bytes: every next one
   is the previous xor the one Nk words back, and every Nk-th goes
   through a rotation, the substitution and one round constant. */
static inline void aes_key(struct aes *a, const uint8_t *key, int bits)
{
    int nk = bits / 32;                 /* 4, 6 or 8 words of key */
    int words;
    uint8_t rcon = 1;

    a->rounds = nk + 6;
    words = 4 * (a->rounds + 1);

    for (int i = 0; i < nk * 4; i++)
        a->rk[i] = key[i];

    for (int i = nk; i < words; i++) {
        uint8_t t[4];
        for (int j = 0; j < 4; j++)
            t[j] = a->rk[(i - 1) * 4 + j];

        if (i % nk == 0) {
            uint8_t tmp = t[0];         /* RotWord */
            t[0] = aes_sbox[t[1]];
            t[1] = aes_sbox[t[2]];
            t[2] = aes_sbox[t[3]];
            t[3] = aes_sbox[tmp];
            t[0] ^= rcon;
            rcon = aes_xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            /* AES-256 only: the substitution without the rotation.
               Forgetting this line yields an AES that agrees with
               itself and with nobody else. */
            for (int j = 0; j < 4; j++)
                t[j] = aes_sbox[t[j]];
        }

        for (int j = 0; j < 4; j++)
            a->rk[i * 4 + j] = a->rk[(i - nk) * 4 + j] ^ t[j];
    }
}

static inline void aes_add_key(uint8_t s[16], const uint8_t *rk)
{
    for (int i = 0; i < 16; i++)
        s[i] ^= rk[i];
}

/* The state is written down by columns: s[0..3] is the first column.
   ShiftRows therefore moves bytes 1, 5, 9, 13 - a row, not a column. */
static inline void aes_shift_rows(uint8_t s[16])
{
    uint8_t t;

    t = s[1];  s[1] = s[5];  s[5] = s[9];  s[9] = s[13]; s[13] = t;
    t = s[2];  s[2] = s[10]; s[10] = t;
    t = s[6];  s[6] = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}

static inline void aes_inv_shift_rows(uint8_t s[16])
{
    uint8_t t;

    t = s[13]; s[13] = s[9];  s[9] = s[5];  s[5] = s[1];  s[1] = t;
    t = s[2];  s[2] = s[10]; s[10] = t;
    t = s[6];  s[6] = s[14]; s[14] = t;
    t = s[3];  s[3] = s[7];  s[7] = s[11]; s[11] = s[15]; s[15] = t;
}

static inline void aes_mix_columns(uint8_t s[16])
{
    for (int c = 0; c < 16; c += 4) {
        uint8_t a0 = s[c], a1 = s[c + 1], a2 = s[c + 2], a3 = s[c + 3];
        uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);

        s[c]     = (uint8_t)(a0 ^ all ^ aes_xtime((uint8_t)(a0 ^ a1)));
        s[c + 1] = (uint8_t)(a1 ^ all ^ aes_xtime((uint8_t)(a1 ^ a2)));
        s[c + 2] = (uint8_t)(a2 ^ all ^ aes_xtime((uint8_t)(a2 ^ a3)));
        s[c + 3] = (uint8_t)(a3 ^ all ^ aes_xtime((uint8_t)(a3 ^ a0)));
    }
}

static inline void aes_inv_mix_columns(uint8_t s[16])
{
    for (int c = 0; c < 16; c += 4) {
        uint8_t a0 = s[c], a1 = s[c + 1], a2 = s[c + 2], a3 = s[c + 3];

        s[c]     = (uint8_t)(aes_mul(a0, 14) ^ aes_mul(a1, 11) ^
                             aes_mul(a2, 13) ^ aes_mul(a3, 9));
        s[c + 1] = (uint8_t)(aes_mul(a0, 9) ^ aes_mul(a1, 14) ^
                             aes_mul(a2, 11) ^ aes_mul(a3, 13));
        s[c + 2] = (uint8_t)(aes_mul(a0, 13) ^ aes_mul(a1, 9) ^
                             aes_mul(a2, 14) ^ aes_mul(a3, 11));
        s[c + 3] = (uint8_t)(aes_mul(a0, 11) ^ aes_mul(a1, 13) ^
                             aes_mul(a2, 9) ^ aes_mul(a3, 14));
    }
}

static inline void aes_encrypt_block(const struct aes *a, const uint8_t in[16],
                                     uint8_t out[16])
{
    uint8_t s[16];

    for (int i = 0; i < 16; i++)
        s[i] = in[i];

    aes_add_key(s, a->rk);
    for (int r = 1; r < a->rounds; r++) {
        for (int i = 0; i < 16; i++)
            s[i] = aes_sbox[s[i]];
        aes_shift_rows(s);
        aes_mix_columns(s);
        aes_add_key(s, a->rk + r * 16);
    }
    /* The last round has no MixColumns - so that decryption can be
       written as the mirror image of encryption. */
    for (int i = 0; i < 16; i++)
        s[i] = aes_sbox[s[i]];
    aes_shift_rows(s);
    aes_add_key(s, a->rk + a->rounds * 16);

    for (int i = 0; i < 16; i++)
        out[i] = s[i];
}

static inline void aes_decrypt_block(const struct aes *a, const uint8_t in[16],
                                     uint8_t out[16])
{
    uint8_t s[16];

    for (int i = 0; i < 16; i++)
        s[i] = in[i];

    aes_add_key(s, a->rk + a->rounds * 16);
    for (int r = a->rounds - 1; r > 0; r--) {
        aes_inv_shift_rows(s);
        for (int i = 0; i < 16; i++)
            s[i] = aes_inv_sbox[s[i]];
        aes_add_key(s, a->rk + r * 16);
        aes_inv_mix_columns(s);
    }
    aes_inv_shift_rows(s);
    for (int i = 0; i < 16; i++)
        s[i] = aes_inv_sbox[s[i]];
    aes_add_key(s, a->rk);

    for (int i = 0; i < 16; i++)
        out[i] = s[i];
}

/* --- CTR (SP 800-38A) -------------------------------------------------
 *
 * The block cipher turned into a stream one: we encrypt successive
 * counter values and xor them onto the data. The counter block is
 * counted up as one big-endian number over all sixteen bytes.
 */
static inline void aes_ctr_inc(uint8_t ctr[16], int from)
{
    for (int i = 15; i >= from; i--)
        if (++ctr[i])
            break;
}

static inline void aes_ctr(const struct aes *a, uint8_t ctr[16],
                           const uint8_t *in, uint8_t *out, uint32_t len)
{
    uint8_t block[16];

    while (len) {
        uint32_t n = len < 16 ? len : 16;
        aes_encrypt_block(a, ctr, block);
        for (uint32_t i = 0; i < n; i++)
            out[i] = (uint8_t)(in[i] ^ block[i]);
        aes_ctr_inc(ctr, 0);
        in += n;
        out += n;
        len -= n;
    }
}

/* --- GHASH and GCM (SP 800-38D) ---------------------------------------
 *
 * GHASH is a sum in GF(2^128): every block is added to the running
 * value and the whole is multiplied by H = E(K, 0). The multiplication
 * is done bit by bit - one hundred and twenty-eight steps per block,
 * with no precomputed tables. Slow, but there is nothing in it to get
 * wrong beyond the reduction constant 0xe1.
 */
static inline void ghash_mul(uint8_t y[16], const uint8_t h[16])
{
    uint8_t z[16] = {0};
    uint8_t v[16];

    for (int i = 0; i < 16; i++)
        v[i] = h[i];

    for (int i = 0; i < 128; i++) {
        if (y[i / 8] & (0x80 >> (i % 8)))
            for (int j = 0; j < 16; j++)
                z[j] ^= v[j];

        /* V = V >> 1, and when a one falls off the bottom, the
           reduction: xor 0xe1 into the topmost byte. */
        int carry = v[15] & 1;
        for (int j = 15; j > 0; j--)
            v[j] = (uint8_t)((v[j] >> 1) | (v[j - 1] << 7));
        v[0] >>= 1;
        if (carry)
            v[0] ^= 0xe1;
    }

    for (int i = 0; i < 16; i++)
        y[i] = z[i];
}

static inline void ghash_update(uint8_t y[16], const uint8_t h[16],
                                const uint8_t *data, uint32_t len)
{
    while (len) {
        uint32_t n = len < 16 ? len : 16;
        for (uint32_t i = 0; i < n; i++)
            y[i] ^= data[i];
        /* A short final piece counts as zero-padded */
        ghash_mul(y, h);
        data += n;
        len -= n;
    }
}

static inline void aes_put64be(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static inline void gcm_tag(const struct aes *a, const uint8_t h[16],
                           const uint8_t j0[16], const uint8_t *aad,
                           uint32_t aadlen, const uint8_t *ct, uint32_t ctlen,
                           uint8_t *tag, uint32_t taglen)
{
    uint8_t y[16] = {0};
    uint8_t lens[16];
    uint8_t s[16];

    ghash_update(y, h, aad, aadlen);
    ghash_update(y, h, ct, ctlen);
    aes_put64be(lens, (uint64_t)aadlen * 8);
    aes_put64be(lens + 8, (uint64_t)ctlen * 8);
    ghash_update(y, h, lens, 16);

    aes_encrypt_block(a, j0, s);
    for (uint32_t i = 0; i < taglen && i < 16; i++)
        tag[i] = (uint8_t)(y[i] ^ s[i]);
}

/* J0 - where the counter starts. A twelve-byte nonce (the usual one)
   simply gets 1 appended; any other length is hashed with GHASH,
   because the counter has to be a block. */
static inline void gcm_j0(const uint8_t h[16], const uint8_t *nonce,
                          uint32_t noncelen, uint8_t j0[16])
{
    if (noncelen == 12) {
        for (int i = 0; i < 12; i++)
            j0[i] = nonce[i];
        j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
        return;
    }

    uint8_t lens[16] = {0};
    for (int i = 0; i < 16; i++)
        j0[i] = 0;
    ghash_update(j0, h, nonce, noncelen);
    aes_put64be(lens + 8, (uint64_t)noncelen * 8);
    ghash_update(j0, h, lens, 16);
}


/* The data start at J0+1: J0 itself is kept for the tag. Only the last
   four bytes are counted up - the rest of the block is the nonce and
   must not move, which is exactly what makes GCM's counter different
   from the plain CTR one above. */
static inline void gcm_crypt(const struct aes *a, const uint8_t j0[16],
                             const uint8_t *in, uint8_t *out, uint32_t len)
{
    uint8_t ctr[16];

    for (int i = 0; i < 16; i++)
        ctr[i] = j0[i];

    while (len) {
        uint32_t n = len < 16 ? len : 16;
        uint8_t block[16];

        aes_ctr_inc(ctr, 12);
        aes_encrypt_block(a, ctr, block);
        for (uint32_t i = 0; i < n; i++)
            out[i] = (uint8_t)(in[i] ^ block[i]);
        in += n;
        out += n;
        len -= n;
    }
}

static inline void aes_gcm_encrypt(const uint8_t *key, int keybits,
                                   const uint8_t *nonce, uint32_t noncelen,
                                   const uint8_t *aad, uint32_t aadlen,
                                   const uint8_t *pt, uint8_t *ct,
                                   uint32_t len, uint8_t *tag, uint32_t taglen)
{
    struct aes a;
    uint8_t h[16] = {0};
    uint8_t j0[16];

    aes_key(&a, key, keybits);
    aes_encrypt_block(&a, h, h);            /* H = E(K, 0^128) */
    gcm_j0(h, nonce, noncelen, j0);

    gcm_crypt(&a, j0, pt, ct, len);
    /* The tag counts the CIPHERtext, not the plaintext - that is what
       lets the receiver check it before decrypting anything. */
    gcm_tag(&a, h, j0, aad, aadlen, ct, len, tag, taglen);
}

/* 1 = the tag agrees and 'pt' holds the message, 0 = forgery (and then
   nothing is written to 'pt'). */
static inline int aes_gcm_decrypt(const uint8_t *key, int keybits,
                                  const uint8_t *nonce, uint32_t noncelen,
                                  const uint8_t *aad, uint32_t aadlen,
                                  const uint8_t *ct, uint8_t *pt,
                                  uint32_t len, const uint8_t *tag,
                                  uint32_t taglen)
{
    struct aes a;
    uint8_t h[16] = {0};
    uint8_t j0[16], want[16];
    uint8_t diff = 0;

    aes_key(&a, key, keybits);
    aes_encrypt_block(&a, h, h);
    gcm_j0(h, nonce, noncelen, j0);
    gcm_tag(&a, h, j0, aad, aadlen, ct, len, want, taglen);

    /* The comparison runs over every byte on purpose: stopping at the
       first difference would tell the attacker how much of a forged tag
       was right, and that is enough to guess the rest byte by byte. */
    for (uint32_t i = 0; i < taglen; i++)
        diff |= (uint8_t)(want[i] ^ tag[i]);
    if (diff)
        return 0;

    gcm_crypt(&a, j0, ct, pt, len);
    return 1;
}

/* --- CCM (SP 800-38C, RFC 3610) ---------------------------------------
 *
 * Two passes over the same key: CBC-MAC computes the tag, CTR encrypts
 * the data and the tag. WPA2 uses exactly this, under the name CCMP,
 * with a thirteen-byte nonce (so L=2) and an eight-byte tag.
 *
 * The whole difficulty of CCM is in the first block: the flags say how
 * long the tag is, whether there is associated data, and how many bytes
 * the length field takes - and the length of the message is written
 * into the SAME block. Because of that CCM cannot start until the
 * length of the message is known; a stream it is not.
 */
static inline void ccm_mac_block(const struct aes *a, uint8_t x[16],
                                 const uint8_t *b)
{
    for (int i = 0; i < 16; i++)
        x[i] ^= b[i];
    aes_encrypt_block(a, x, x);
}

static inline void ccm_ctr_block(uint8_t ctr[16], const uint8_t *nonce,
                                 uint32_t noncelen, uint32_t counter)
{
    uint32_t L = 15 - noncelen;

    ctr[0] = (uint8_t)(L - 1);
    for (uint32_t i = 0; i < noncelen; i++)
        ctr[1 + i] = nonce[i];
    /* The counter goes into L bytes, and L reaches eight - so the shift
       has to be done on sixty-four bits. On thirty-two it would be
       undefined at the ninth byte, which is the kind of thing that
       works until somebody uses a short nonce. */
    for (uint32_t i = 0; i < L; i++)
        ctr[15 - i] = (uint8_t)((uint64_t)counter >> (8 * i));
}

/* The MAC over B0, the associated data and the message. Returns the
   first 'taglen' bytes of the CBC-MAC - still unencrypted. */
static inline void ccm_mac(const struct aes *a, const uint8_t *nonce,
                           uint32_t noncelen, const uint8_t *aad,
                           uint32_t aadlen, const uint8_t *pt, uint32_t len,
                           uint32_t taglen, uint8_t mac[16])
{
    uint32_t L = 15 - noncelen;
    uint8_t b[16] = {0};
    uint8_t x[16] = {0};

    b[0] = (uint8_t)((aadlen ? 0x40 : 0) |
                     (((taglen - 2) / 2) << 3) |
                     (L - 1));
    for (uint32_t i = 0; i < noncelen; i++)
        b[1 + i] = nonce[i];
    for (uint32_t i = 0; i < L; i++)
        b[15 - i] = (uint8_t)((uint64_t)len >> (8 * i));
    ccm_mac_block(a, x, b);

    if (aadlen) {
        uint32_t k = 0;

        for (int i = 0; i < 16; i++)
            b[i] = 0;
        /* The length of the associated data goes in front of it, and
           its encoding depends on how long it is. We handle the two
           lengths a network packet can bring. */
        if (aadlen < 0xFF00) {
            b[0] = (uint8_t)(aadlen >> 8);
            b[1] = (uint8_t)aadlen;
            k = 2;
        } else {
            b[0] = 0xFF;
            b[1] = 0xFE;
            b[2] = (uint8_t)(aadlen >> 24);
            b[3] = (uint8_t)(aadlen >> 16);
            b[4] = (uint8_t)(aadlen >> 8);
            b[5] = (uint8_t)aadlen;
            k = 6;
        }
        for (uint32_t i = 0; i < aadlen; i++) {
            if (k == 16) {
                ccm_mac_block(a, x, b);
                for (int j = 0; j < 16; j++)
                    b[j] = 0;
                k = 0;
            }
            b[k++] = aad[i];
        }
        if (k)
            ccm_mac_block(a, x, b);      /* the rest padded with zeros */
    }

    for (uint32_t off = 0; off < len; off += 16) {
        uint32_t n = len - off < 16 ? len - off : 16;

        for (int i = 0; i < 16; i++)
            b[i] = 0;
        for (uint32_t i = 0; i < n; i++)
            b[i] = pt[off + i];
        ccm_mac_block(a, x, b);
    }

    for (int i = 0; i < 16; i++)
        mac[i] = x[i];
}

/* noncelen 7..13 (L = 15 - noncelen), taglen even, 4..16.
   Returns 0 when the parameters do not make sense. */
static inline int aes_ccm_encrypt(const uint8_t *key, int keybits,
                                  const uint8_t *nonce, uint32_t noncelen,
                                  const uint8_t *aad, uint32_t aadlen,
                                  const uint8_t *pt, uint8_t *ct,
                                  uint32_t len, uint8_t *tag, uint32_t taglen)
{
    struct aes a;
    uint8_t mac[16], s0[16], ctr[16];

    if (noncelen < 7 || noncelen > 13 || taglen < 4 || taglen > 16 ||
        (taglen & 1))
        return 0;

    aes_key(&a, key, keybits);
    ccm_mac(&a, nonce, noncelen, aad, aadlen, pt, len, taglen, mac);

    /* Counter 0 encrypts the tag, the data start at counter 1. */
    ccm_ctr_block(ctr, nonce, noncelen, 0);
    aes_encrypt_block(&a, ctr, s0);

    for (uint32_t off = 0; off < len; off += 16) {
        uint32_t n = len - off < 16 ? len - off : 16;
        uint8_t block[16];

        ccm_ctr_block(ctr, nonce, noncelen, off / 16 + 1);
        aes_encrypt_block(&a, ctr, block);
        for (uint32_t i = 0; i < n; i++)
            ct[off + i] = (uint8_t)(pt[off + i] ^ block[i]);
    }

    for (uint32_t i = 0; i < taglen; i++)
        tag[i] = (uint8_t)(mac[i] ^ s0[i]);
    return 1;
}

/* 1 = the tag agrees, 0 = forgery or wrong parameters. */
static inline int aes_ccm_decrypt(const uint8_t *key, int keybits,
                                  const uint8_t *nonce, uint32_t noncelen,
                                  const uint8_t *aad, uint32_t aadlen,
                                  const uint8_t *ct, uint8_t *pt,
                                  uint32_t len, const uint8_t *tag,
                                  uint32_t taglen)
{
    struct aes a;
    uint8_t mac[16], s0[16], ctr[16];
    uint8_t diff = 0;

    if (noncelen < 7 || noncelen > 13 || taglen < 4 || taglen > 16 ||
        (taglen & 1))
        return 0;

    aes_key(&a, key, keybits);
    ccm_ctr_block(ctr, nonce, noncelen, 0);
    aes_encrypt_block(&a, ctr, s0);

    /* CCM has to decrypt before it can check: the MAC counts the
       PLAINtext. So the message is deciphered into the caller's buffer
       and only then rejected - and that is why 'pt' is wiped when the
       tag does not agree. Handing back something unverified would be
       worse than handing back nothing. */
    for (uint32_t off = 0; off < len; off += 16) {
        uint32_t n = len - off < 16 ? len - off : 16;
        uint8_t block[16];

        ccm_ctr_block(ctr, nonce, noncelen, off / 16 + 1);
        aes_encrypt_block(&a, ctr, block);
        for (uint32_t i = 0; i < n; i++)
            pt[off + i] = (uint8_t)(ct[off + i] ^ block[i]);
    }

    ccm_mac(&a, nonce, noncelen, aad, aadlen, pt, len, taglen, mac);
    for (uint32_t i = 0; i < taglen; i++)
        diff |= (uint8_t)((mac[i] ^ s0[i]) ^ tag[i]);
    if (diff) {
        for (uint32_t i = 0; i < len; i++)
            pt[i] = 0;
        return 0;
    }
    return 1;
}

/* --- Key wrap (RFC 3394) ---------------------------------------------
 *
 * How one key travels inside another. WPA2 sends the group key this
 * way in the third message of the handshake, wrapped with the KEK.
 *
 * The trick of it is the constant A6A6A6A6A6A6A6A6: unwrapping ends
 * with a comparison against it, so a wrong key does not yield rubbish
 * that looks like a key - it yields a refusal. Six passes over the
 * blocks mix every block into every other one, which is why a single
 * changed bit anywhere makes the whole thing fail rather than damage
 * one block.
 */
static inline void aes_wrap(const uint8_t *kek, int keybits,
                            const uint8_t *in, uint32_t blocks, uint8_t *out)
{
    struct aes a;
    uint8_t v[16], av[8];

    aes_key(&a, kek, keybits);
    for (int i = 0; i < 8; i++)
        av[i] = 0xA6;
    for (uint32_t i = 0; i < blocks * 8; i++)
        out[8 + i] = in[i];

    for (uint32_t j = 0; j < 6; j++) {
        for (uint32_t i = 1; i <= blocks; i++) {
            uint64_t t = (uint64_t)blocks * j + i;

            for (int k = 0; k < 8; k++) {
                v[k] = av[k];
                v[8 + k] = out[i * 8 + k];
            }
            aes_encrypt_block(&a, v, v);
            for (int k = 0; k < 8; k++) {
                av[k] = v[k];
                out[i * 8 + k] = v[8 + k];
            }
            for (int k = 0; k < 8; k++)
                av[7 - k] ^= (uint8_t)(t >> (8 * k));
        }
    }

    for (int i = 0; i < 8; i++)
        out[i] = av[i];
}

/* 1 = unwrapped and the constant agrees, 0 = the key is wrong or the
   data are damaged. */
static inline int aes_unwrap(const uint8_t *kek, int keybits,
                             const uint8_t *in, uint32_t blocks, uint8_t *out)
{
    struct aes a;
    uint8_t v[16], av[8];

    aes_key(&a, kek, keybits);
    for (int i = 0; i < 8; i++)
        av[i] = in[i];
    for (uint32_t i = 0; i < blocks * 8; i++)
        out[i] = in[8 + i];

    for (int j = 5; j >= 0; j--) {
        for (uint32_t i = blocks; i >= 1; i--) {
            uint64_t t = (uint64_t)blocks * (uint32_t)j + i;

            for (int k = 0; k < 8; k++)
                v[k] = (uint8_t)(av[k] ^ (uint8_t)(t >> (8 * (7 - k))));
            for (int k = 0; k < 8; k++)
                v[8 + k] = out[(i - 1) * 8 + k];
            aes_decrypt_block(&a, v, v);
            for (int k = 0; k < 8; k++) {
                av[k] = v[k];
                out[(i - 1) * 8 + k] = v[8 + k];
            }
        }
    }

    uint8_t diff = 0;
    for (int i = 0; i < 8; i++)
        diff |= (uint8_t)(av[i] ^ 0xA6);
    return diff == 0;
}

#endif