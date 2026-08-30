/* Big number arithmetic for RSA (Montgomery modular multiplication)
 *
 * To verify an RSA signature, modular exponentiation with a small
 * public exponent (usually 65537) is enough. We use the Montgomery
 * form, because it makes it possible to compute a remainder with
 * multiplications and shifts alone - division of big numbers then does
 * not have to be written at all.
 *
 * Numbers are kept as arrays of 32-bit limbs, least significant first.
 * 2048 bits = 64 limbs; with room to spare for 4096-bit keys.
 */

#ifndef PROTONOS_UBIGNUM_H
#define PROTONOS_UBIGNUM_H

#include "uapi.h"

#define BN_MAX_LIMBS 128                 /* up to 4096 bits */

struct bignum {
    uint32_t n[BN_MAX_LIMBS];
    int len;                             /* the number of limbs in use */
};

static inline void bn_zero(struct bignum *a)
{
    for (int i = 0; i < BN_MAX_LIMBS; i++)
        a->n[i] = 0;
    a->len = 0;
}

static inline void bn_trim(struct bignum *a)
{
    while (a->len > 0 && a->n[a->len - 1] == 0)
        a->len--;
}

/* Reads a number from a big-endian representation (as they lie in a
   certificate) */
static inline int bn_from_bytes(struct bignum *a, const uint8_t *p, uint32_t len)
{
    while (len > 0 && *p == 0) {         /* leading zeros contribute nothing */
        p++;
        len--;
    }
    if (len > BN_MAX_LIMBS * 4)
        return -1;

    bn_zero(a);
    a->len = (int)((len + 3) / 4);
    for (uint32_t i = 0; i < len; i++) {
        uint32_t byte_from_end = len - 1 - i;
        a->n[byte_from_end / 4] |= (uint32_t)p[i] << ((byte_from_end % 4) * 8);
    }
    bn_trim(a);
    return 0;
}

/* Writes a number big-endian into exactly 'len' bytes (with zeros) */
static inline void bn_to_bytes(const struct bignum *a, uint8_t *out,
                               uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        uint32_t byte_from_end = len - 1 - i;
        uint32_t limb = byte_from_end / 4;
        out[i] = (limb < (uint32_t)a->len)
                 ? (uint8_t)(a->n[limb] >> ((byte_from_end % 4) * 8))
                 : 0;
    }
}

static inline int bn_cmp(const struct bignum *a, const struct bignum *b)
{
    int la = a->len, lb = b->len;
    if (la != lb)
        return la > lb ? 1 : -1;
    for (int i = la - 1; i >= 0; i--)
        if (a->n[i] != b->n[i])
            return a->n[i] > b->n[i] ? 1 : -1;
    return 0;
}

/* a = a - b (assumes a >= b) */
static inline void bn_sub(struct bignum *a, const struct bignum *b)
{
    uint64_t borrow = 0;
    for (int i = 0; i < a->len; i++) {
        uint64_t bi = (i < b->len) ? b->n[i] : 0;
        uint64_t cur = (uint64_t)a->n[i] - bi - borrow;
        a->n[i] = (uint32_t)cur;
        borrow = (cur >> 63) & 1;        /* a borrow, when the result is negative */
    }
    bn_trim(a);
}

/* The inverse of -n[0] modulo 2^32 (needed in REDC) */
static inline uint32_t bn_inv32(uint32_t n0)
{
    uint32_t x = 1;
    for (int i = 0; i < 5; i++)          /* Newton: it doubles the number of bits */
        x *= 2 - n0 * x;
    return (uint32_t)(0 - x);
}

/* Montgomery multiplication: out = a * b * R^-1 mod m */
static inline void bn_mont_mul(struct bignum *out, const struct bignum *a,
                               const struct bignum *b, const struct bignum *m,
                               uint32_t m_inv)
{
    uint32_t t[BN_MAX_LIMBS * 2 + 2];
    int n = m->len;

    for (int i = 0; i < 2 * n + 2; i++)
        t[i] = 0;

    for (int i = 0; i < n; i++) {
        uint64_t carry = 0;
        uint32_t ai = (i < a->len) ? a->n[i] : 0;

        for (int j = 0; j < n; j++) {    /* t += a[i] * b */
            uint32_t bj = (j < b->len) ? b->n[j] : 0;
            uint64_t cur = (uint64_t)ai * bj + t[i + j] + carry;
            t[i + j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        uint64_t sum = (uint64_t)t[i + n] + carry;
        t[i + n] = (uint32_t)sum;
        t[i + n + 1] += (uint32_t)(sum >> 32);

        /* zeroing the lowest limb: t += (t[i] * m_inv) * m */
        uint32_t u = t[i] * m_inv;
        carry = 0;
        for (int j = 0; j < n; j++) {
            uint64_t cur = (uint64_t)u * m->n[j] + t[i + j] + carry;
            t[i + j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        sum = (uint64_t)t[i + n] + carry;
        t[i + n] = (uint32_t)sum;
        t[i + n + 1] += (uint32_t)(sum >> 32);
    }

    bn_zero(out);
    out->len = n + 1;
    for (int i = 0; i <= n; i++)
        out->n[i] = t[n + i];
    bn_trim(out);

    if (bn_cmp(out, m) >= 0)
        bn_sub(out, m);
}

/* a = a + b (mod nothing - a plain sum; assumes room for a carry) */
static inline void bn_add_raw(struct bignum *a, const struct bignum *b)
{
    int len = a->len > b->len ? a->len : b->len;
    uint64_t carry = 0;

    for (int i = 0; i < len; i++) {
        uint64_t ai = (i < a->len) ? a->n[i] : 0;
        uint64_t bi = (i < b->len) ? b->n[i] : 0;
        uint64_t cur = ai + bi + carry;
        a->n[i] = (uint32_t)cur;
        carry = cur >> 32;
    }
    a->len = len;
    if (carry && a->len < BN_MAX_LIMBS)
        a->n[a->len++] = (uint32_t)carry;
}

/* Modular addition and subtraction (the values have to be reduced
   already). The result is built in a working copy, because the caller
   often passes the same object as the result and an argument (say
   fp_add(&a, &b, &a)) - writing straight into 'out' would then destroy
   the second argument. */
static inline void bn_add_mod(struct bignum *out, const struct bignum *a,
                              const struct bignum *b, const struct bignum *m)
{
    struct bignum t = *a, u = *b;

    bn_add_raw(&t, &u);
    if (bn_cmp(&t, m) >= 0)
        bn_sub(&t, m);
    *out = t;
}

static inline void bn_sub_mod(struct bignum *out, const struct bignum *a,
                              const struct bignum *b, const struct bignum *m)
{
    struct bignum t = *a, u = *b;

    if (bn_cmp(&t, &u) < 0) {            /* borrow the modulus */
        struct bignum mm = *m;
        bn_add_raw(&t, &mm);
    }
    bn_sub(&t, &u);
    *out = t;
}

/* Modular exponentiation: out = base^exp mod m (a 32-bit exponent) */
static inline int bn_modexp(struct bignum *out, const struct bignum *base,
                            uint32_t exp, const struct bignum *m)
{
    struct bignum r2, x, acc, one;

    if (m->len == 0 || !(m->n[0] & 1))   /* the modulus has to be odd */
        return -1;

    uint32_t m_inv = bn_inv32(m->n[0]);
    int n = m->len;

    /* R^2 mod m we compute by doubling one 64n times: 2^(64n) mod m.
       Every doubling ends in at most one subtraction of the modulus, so
       no division of big numbers is needed here. */
    bn_zero(&r2);
    r2.len = 1;
    r2.n[0] = 1;

    for (int i = 0; i < 64 * n; i++) {
        uint32_t carry = 0;
        for (int j = 0; j < r2.len; j++) {
            uint32_t v = r2.n[j];
            r2.n[j] = (v << 1) | carry;
            carry = v >> 31;
        }
        if (carry && r2.len < BN_MAX_LIMBS)
            r2.n[r2.len++] = carry;
        if (bn_cmp(&r2, m) >= 0)
            bn_sub(&r2, m);
    }

    /* the move into Montgomery form */
    bn_mont_mul(&x, base, &r2, m, m_inv);

    bn_zero(&one);
    one.len = 1;
    one.n[0] = 1;
    bn_mont_mul(&acc, &one, &r2, m, m_inv);   /* acc = 1 in Montgomery form */

    int started = 0;
    for (int i = 31; i >= 0; i--) {
        if (started)
            bn_mont_mul(&acc, &acc, &acc, m, m_inv);
        if ((exp >> i) & 1) {
            if (started)
                bn_mont_mul(&acc, &acc, &x, m, m_inv);
            else {
                acc = x;
                started = 1;
            }
        }
    }

    bn_mont_mul(out, &acc, &one, m, m_inv);   /* the return from Montgomery form */
    return 0;
}

/* Exponentiation with an exponent of any length (big-endian). Needed
   for modular inversion in the curve's field: a^(p-2). */
static inline int bn_modexp_bytes(struct bignum *out, const struct bignum *base,
                                  const uint8_t *exp, uint32_t explen,
                                  const struct bignum *m)
{
    struct bignum r2, x, acc, one;

    if (m->len == 0 || !(m->n[0] & 1))
        return -1;

    uint32_t m_inv = bn_inv32(m->n[0]);
    int n = m->len;

    bn_zero(&r2);
    r2.len = 1;
    r2.n[0] = 1;
    for (int i = 0; i < 64 * n; i++) {
        uint32_t carry = 0;
        for (int j = 0; j < r2.len; j++) {
            uint32_t v = r2.n[j];
            r2.n[j] = (v << 1) | carry;
            carry = v >> 31;
        }
        if (carry && r2.len < BN_MAX_LIMBS)
            r2.n[r2.len++] = carry;
        if (bn_cmp(&r2, m) >= 0)
            bn_sub(&r2, m);
    }

    bn_mont_mul(&x, base, &r2, m, m_inv);

    bn_zero(&one);
    one.len = 1;
    one.n[0] = 1;
    bn_mont_mul(&acc, &one, &r2, m, m_inv);

    int started = 0;
    for (uint32_t i = 0; i < explen; i++) {
        for (int b = 7; b >= 0; b--) {
            if (started)
                bn_mont_mul(&acc, &acc, &acc, m, m_inv);
            if ((exp[i] >> b) & 1) {
                if (started) {
                    bn_mont_mul(&acc, &acc, &x, m, m_inv);
                } else {
                    acc = x;
                    started = 1;
                }
            }
        }
    }
    if (!started) {                      /* a zero exponent -> 1 */
        bn_zero(out);
        out->len = 1;
        out->n[0] = 1;
        return 0;
    }

    bn_mont_mul(out, &acc, &one, m, m_inv);
    return 0;
}

static inline int bn_is_zero(const struct bignum *a)
{
    return a->len == 0;
}

#endif
