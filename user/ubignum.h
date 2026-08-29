/* Arytmetyka wielkich liczb dla RSA (mnozenie modularne Montgomery'ego)
 *
 * Do sprawdzenia podpisu RSA wystarczy potegowanie modularne z malym
 * wykladnikiem publicznym (zwykle 65537). Uzywamy postaci
 * Montgomery'ego, bo pozwala liczyc reszte z dzielenia samymi
 * mnozeniami i przesunieciami - dzielenia wielkich liczb nie trzeba
 * wtedy pisac wcale.
 *
 * Liczby trzymamy jako tablice 32-bitowych czlonow, od najmniej
 * znaczacego. 2048 bitow = 64 czlony; z zapasem na 4096-bitowe klucze.
 */

#ifndef PROTONOS_UBIGNUM_H
#define PROTONOS_UBIGNUM_H

#include "uapi.h"

#define BN_MAX_LIMBS 128                 /* do 4096 bitow */

struct bignum {
    uint32_t n[BN_MAX_LIMBS];
    int len;                             /* liczba uzywanych czlonow */
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

/* Wczytuje liczbe z zapisu big-endian (tak jak leza w certyfikacie) */
static inline int bn_from_bytes(struct bignum *a, const uint8_t *p, uint32_t len)
{
    while (len > 0 && *p == 0) {         /* wiodace zera nic nie wnosza */
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

/* Zapisuje liczbe big-endian na dokladnie 'len' bajtach (z zerami) */
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

/* a = a - b (zaklada a >= b) */
static inline void bn_sub(struct bignum *a, const struct bignum *b)
{
    uint64_t borrow = 0;
    for (int i = 0; i < a->len; i++) {
        uint64_t bi = (i < b->len) ? b->n[i] : 0;
        uint64_t cur = (uint64_t)a->n[i] - bi - borrow;
        a->n[i] = (uint32_t)cur;
        borrow = (cur >> 63) & 1;        /* pozyczka, gdy wynik ujemny */
    }
    bn_trim(a);
}

/* Odwrotnosc -n[0] modulo 2^32 (potrzebna w REDC) */
static inline uint32_t bn_inv32(uint32_t n0)
{
    uint32_t x = 1;
    for (int i = 0; i < 5; i++)          /* Newton: podwaja liczbe bitow */
        x *= 2 - n0 * x;
    return (uint32_t)(0 - x);
}

/* Mnozenie Montgomery'ego: out = a * b * R^-1 mod m */
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

        /* zerowanie najnizszego czlonu: t += (t[i] * m_inv) * m */
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

/* a = a + b (mod nic - czysta suma; zaklada zapas na przeniesienie) */
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

/* Dodawanie i odejmowanie modulo (wartosci musza byc juz zredukowane).
   Wynik powstaje w kopii roboczej, bo wolajacy czesto podaje ten sam
   obiekt jako wynik i argument (np. fp_add(&a, &b, &a)) - zapis wprost
   do 'out' zniszczylby wtedy drugi argument. */
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

    if (bn_cmp(&t, &u) < 0) {            /* pozycz modulus */
        struct bignum mm = *m;
        bn_add_raw(&t, &mm);
    }
    bn_sub(&t, &u);
    *out = t;
}

/* Potegowanie modularne: out = base^exp mod m (wykladnik 32-bitowy) */
static inline int bn_modexp(struct bignum *out, const struct bignum *base,
                            uint32_t exp, const struct bignum *m)
{
    struct bignum r2, x, acc, one;

    if (m->len == 0 || !(m->n[0] & 1))   /* modulus musi byc nieparzysty */
        return -1;

    uint32_t m_inv = bn_inv32(m->n[0]);
    int n = m->len;

    /* R^2 mod m liczymy podwajaniem jedynki 64n razy: 2^(64n) mod m.
       Kazde podwojenie konczy sie najwyzej jednym odjeciem modulusa,
       wiec nie potrzeba tu dzielenia wielkich liczb. */
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

    /* przejscie do postaci Montgomery'ego */
    bn_mont_mul(&x, base, &r2, m, m_inv);

    bn_zero(&one);
    one.len = 1;
    one.n[0] = 1;
    bn_mont_mul(&acc, &one, &r2, m, m_inv);   /* acc = 1 w postaci Montgomery'ego */

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

    bn_mont_mul(out, &acc, &one, m, m_inv);   /* powrot z postaci Montgomery'ego */
    return 0;
}

/* Potegowanie z wykladnikiem dowolnej dlugosci (big-endian).
   Potrzebne do odwrotnosci modulo w ciele krzywej: a^(p-2). */
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
    if (!started) {                      /* wykladnik zerowy -> 1 */
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
