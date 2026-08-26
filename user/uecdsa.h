/* ECDSA na krzywej NIST P-256 (sprawdzanie podpisow)
 *
 * Coraz wiecej serwerow ma certyfikaty na krzywej eliptycznej zamiast
 * RSA - bez tego ProtonOS musial mowic "nieobslugiwany algorytm".
 *
 * Arytmetyka stoi na tych samych wielkich liczbach co RSA: mnozenie
 * Montgomery'ego modulo p. Wspolrzedne punktow trzymamy w postaci
 * Jacobiego (X, Y, Z), gdzie x = X/Z^2, y = Y/Z^3 - dzieki temu
 * dodawanie punktow nie wymaga odwrotnosci, ktora jest droga.
 * Odwrotnosc liczymy raz, na samym koncu.
 *
 * To weryfikacja publicznych danych, wiec kod nie musi byc odporny na
 * pomiar czasu - i nie jest.
 */

#ifndef PROTONOS_UECDSA_H
#define PROTONOS_UECDSA_H

#include "usys.h"
#include "ubignum.h"

/* Parametry krzywej (big-endian) */
static const uint8_t p256_p_bytes[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff };
static const uint8_t p256_n_bytes[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,
    0xfc,0x63,0x25,0x51 };
static const uint8_t p256_gx_bytes[32] = {
    0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,
    0x40,0xf2,0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,
    0xd8,0x98,0xc2,0x96 };
static const uint8_t p256_gy_bytes[32] = {
    0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,
    0x9e,0x16,0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,
    0x37,0xbf,0x51,0xf5 };

/* Punkt w postaci Jacobiego, wspolrzedne w dziedzinie Montgomery'ego */
struct p256_pt {
    struct bignum x, y, z;
    int infinity;
};

static struct bignum p256_p, p256_n, p256_r2p, p256_r2n, p256_one_mont;
static uint32_t p256_pinv, p256_ninv;
static int p256_ready;

/* R^2 mod m - potrzebne do wchodzenia w dziedzine Montgomery'ego */
static inline void p256_calc_r2(struct bignum *r2, const struct bignum *m)
{
    bn_zero(r2);
    r2->len = 1;
    r2->n[0] = 1;
    for (int i = 0; i < 64 * m->len; i++) {
        uint32_t carry = 0;
        for (int j = 0; j < r2->len; j++) {
            uint32_t v = r2->n[j];
            r2->n[j] = (v << 1) | carry;
            carry = v >> 31;
        }
        if (carry && r2->len < BN_MAX_LIMBS)
            r2->n[r2->len++] = carry;
        if (bn_cmp(r2, m) >= 0)
            bn_sub(r2, m);
    }
}

static inline void p256_init(void)
{
    if (p256_ready)
        return;

    bn_from_bytes(&p256_p, p256_p_bytes, 32);
    bn_from_bytes(&p256_n, p256_n_bytes, 32);
    p256_pinv = bn_inv32(p256_p.n[0]);
    p256_ninv = bn_inv32(p256_n.n[0]);
    p256_calc_r2(&p256_r2p, &p256_p);
    p256_calc_r2(&p256_r2n, &p256_n);

    struct bignum one;
    bn_zero(&one);
    one.len = 1;
    one.n[0] = 1;
    bn_mont_mul(&p256_one_mont, &one, &p256_r2p, &p256_p, p256_pinv);

    p256_ready = 1;
}

/* --- ciało modulo p, wartosci w dziedzinie Montgomery'ego --- */

static inline void fp_mul(struct bignum *o, const struct bignum *a,
                          const struct bignum *b)
{
    bn_mont_mul(o, a, b, &p256_p, p256_pinv);
}

static inline void fp_add(struct bignum *o, const struct bignum *a,
                          const struct bignum *b)
{
    bn_add_mod(o, a, b, &p256_p);
}

static inline void fp_sub(struct bignum *o, const struct bignum *a,
                          const struct bignum *b)
{
    bn_sub_mod(o, a, b, &p256_p);
}

static inline void fp_enter(struct bignum *o, const struct bignum *a)
{
    bn_mont_mul(o, a, &p256_r2p, &p256_p, p256_pinv);
}

static inline void fp_leave(struct bignum *o, const struct bignum *a)
{
    struct bignum one;
    bn_zero(&one);
    one.len = 1;
    one.n[0] = 1;
    bn_mont_mul(o, a, &one, &p256_p, p256_pinv);
}

/* Odwrotnosc przez male twierdzenie Fermata: a^(p-2) */
static inline void fp_inv(struct bignum *o, const struct bignum *a)
{
    uint8_t e[32];
    for (int i = 0; i < 32; i++)
        e[i] = p256_p_bytes[i];
    /* p - 2 (p konczy sie na ...ffff, wiec wystarczy odjac od ostatniego) */
    e[31] -= 2;

    struct bignum acc = p256_one_mont;
    struct bignum base = *a;
    int started = 0;

    for (int i = 0; i < 32; i++) {
        for (int b = 7; b >= 0; b--) {
            if (started)
                fp_mul(&acc, &acc, &acc);
            if ((e[i] >> b) & 1) {
                if (started) {
                    fp_mul(&acc, &acc, &base);
                } else {
                    acc = base;
                    started = 1;
                }
            }
        }
    }
    *o = acc;
}

/* --- arytmetyka punktow (Jacobi, a = -3) --- */

static inline void p256_double(struct p256_pt *r, const struct p256_pt *p)
{
    struct bignum delta, gamma, beta, alpha, t1, t2;

    if (p->infinity || bn_is_zero(&p->y)) {
        r->infinity = 1;
        return;
    }

    fp_mul(&delta, &p->z, &p->z);        /* delta = Z^2 */
    fp_mul(&gamma, &p->y, &p->y);        /* gamma = Y^2 */
    fp_mul(&beta, &p->x, &gamma);        /* beta = X*gamma */

    fp_sub(&t1, &p->x, &delta);          /* alpha = 3*(X-delta)*(X+delta) */
    fp_add(&t2, &p->x, &delta);
    fp_mul(&alpha, &t1, &t2);
    fp_add(&t1, &alpha, &alpha);
    fp_add(&alpha, &t1, &alpha);

    fp_add(&t1, &p->y, &p->z);           /* Z3 = (Y+Z)^2 - gamma - delta */
    fp_mul(&t1, &t1, &t1);
    fp_sub(&t1, &t1, &gamma);
    fp_sub(&r->z, &t1, &delta);

    fp_mul(&t1, &alpha, &alpha);         /* X3 = alpha^2 - 8*beta */
    fp_add(&t2, &beta, &beta);
    fp_add(&t2, &t2, &t2);
    fp_add(&t2, &t2, &t2);               /* t2 = 8*beta */
    fp_sub(&t1, &t1, &t2);

    struct bignum x3 = t1;

    fp_add(&t2, &beta, &beta);           /* Y3 = alpha*(4*beta - X3) */
    fp_add(&t2, &t2, &t2);               /*      - 8*gamma^2         */
    fp_sub(&t2, &t2, &x3);
    fp_mul(&t2, &alpha, &t2);

    fp_mul(&t1, &gamma, &gamma);
    fp_add(&t1, &t1, &t1);
    fp_add(&t1, &t1, &t1);
    fp_add(&t1, &t1, &t1);               /* t1 = 8*gamma^2 */
    fp_sub(&r->y, &t2, &t1);

    r->x = x3;
    r->infinity = 0;
}

static inline void p256_add(struct p256_pt *r, const struct p256_pt *p,
                            const struct p256_pt *q)
{
    struct bignum z1z1, z2z2, u1, u2, s1, s2, h, i, j, v, t1, t2, rr;

    if (p->infinity) {
        *r = *q;
        return;
    }
    if (q->infinity) {
        *r = *p;
        return;
    }

    fp_mul(&z1z1, &p->z, &p->z);
    fp_mul(&z2z2, &q->z, &q->z);
    fp_mul(&u1, &p->x, &z2z2);
    fp_mul(&u2, &q->x, &z1z1);

    fp_mul(&t1, &q->z, &z2z2);
    fp_mul(&s1, &p->y, &t1);
    fp_mul(&t2, &p->z, &z1z1);
    fp_mul(&s2, &q->y, &t2);

    fp_sub(&h, &u2, &u1);
    fp_sub(&rr, &s2, &s1);

    if (bn_is_zero(&h)) {
        if (bn_is_zero(&rr)) {
            p256_double(r, p);           /* ten sam punkt */
            return;
        }
        r->infinity = 1;                 /* punkty przeciwne */
        return;
    }

    fp_add(&rr, &rr, &rr);               /* r = 2*(S2-S1) */

    fp_add(&t1, &h, &h);                 /* I = (2H)^2 */
    fp_mul(&i, &t1, &t1);
    fp_mul(&j, &h, &i);                  /* J = H*I */
    fp_mul(&v, &u1, &i);                 /* V = U1*I */

    fp_mul(&t1, &rr, &rr);               /* X3 = r^2 - J - 2V */
    fp_sub(&t1, &t1, &j);
    fp_add(&t2, &v, &v);
    fp_sub(&t1, &t1, &t2);
    struct bignum x3 = t1;

    fp_sub(&t1, &v, &x3);                /* Y3 = r*(V-X3) - 2*S1*J */
    fp_mul(&t1, &rr, &t1);
    fp_mul(&t2, &s1, &j);
    fp_add(&t2, &t2, &t2);
    fp_sub(&r->y, &t1, &t2);

    fp_add(&t1, &p->z, &q->z);           /* Z3 = ((Z1+Z2)^2-Z1Z1-Z2Z2)*H */
    fp_mul(&t1, &t1, &t1);
    fp_sub(&t1, &t1, &z1z1);
    fp_sub(&t1, &t1, &z2z2);
    fp_mul(&r->z, &t1, &h);

    r->x = x3;
    r->infinity = 0;
}

/* r = k*p (podwajaj i dodawaj, od najstarszego bitu) */
static inline void p256_mul(struct p256_pt *r, const struct bignum *k,
                            const struct p256_pt *p)
{
    struct p256_pt acc;
    acc.infinity = 1;
    bn_zero(&acc.x);
    bn_zero(&acc.y);
    bn_zero(&acc.z);

    int started = 0;
    for (int limb = k->len - 1; limb >= 0; limb--) {
        for (int b = 31; b >= 0; b--) {
            if (started)
                p256_double(&acc, &acc);
            if ((k->n[limb] >> b) & 1) {
                if (started) {
                    p256_add(&acc, &acc, p);
                } else {
                    acc = *p;
                    started = 1;
                }
            }
        }
    }
    *r = acc;
}

/* --- podpis --- */

/* Mnozenie modulo n (rzad grupy) - poza dziedzina Montgomery'ego */
static inline void mod_n_mul(struct bignum *o, const struct bignum *a,
                             const struct bignum *b)
{
    struct bignum t;
    bn_mont_mul(&t, a, b, &p256_n, p256_ninv);
    bn_mont_mul(o, &t, &p256_r2n, &p256_n, p256_ninv);
}

/* Sprawdza podpis (r,s) pod skrotem hash dla klucza publicznego
   podanego jako punkt nieskompresowany: 0x04 || X(32) || Y(32).
   1 = podpis dobry. */
static inline int ecdsa_p256_verify(const uint8_t *pub, uint32_t publen,
                                    const uint8_t *hash, const uint8_t *r_bytes,
                                    uint32_t rlen, const uint8_t *s_bytes,
                                    uint32_t slen)
{
    p256_init();

    if (publen != 65 || pub[0] != 0x04)
        return 0;                        /* tylko postac nieskompresowana */

    struct bignum r, s, e, w, u1, u2, qx, qy;
    if (bn_from_bytes(&r, r_bytes, rlen) < 0 ||
        bn_from_bytes(&s, s_bytes, slen) < 0)
        return 0;
    if (bn_is_zero(&r) || bn_is_zero(&s))
        return 0;
    if (bn_cmp(&r, &p256_n) >= 0 || bn_cmp(&s, &p256_n) >= 0)
        return 0;

    bn_from_bytes(&e, hash, 32);
    if (bn_cmp(&e, &p256_n) >= 0)
        bn_sub(&e, &p256_n);

    /* w = s^-1 mod n */
    uint8_t nm2[32];
    for (int i = 0; i < 32; i++)
        nm2[i] = p256_n_bytes[i];
    nm2[31] -= 2;
    if (bn_modexp_bytes(&w, &s, nm2, 32, &p256_n) < 0)
        return 0;

    mod_n_mul(&u1, &e, &w);
    mod_n_mul(&u2, &r, &w);

    /* punkty: G i klucz publiczny */
    struct p256_pt g, q, t1, t2, sum;
    struct bignum tmp;

    bn_from_bytes(&tmp, p256_gx_bytes, 32);
    fp_enter(&g.x, &tmp);
    bn_from_bytes(&tmp, p256_gy_bytes, 32);
    fp_enter(&g.y, &tmp);
    g.z = p256_one_mont;
    g.infinity = 0;

    if (bn_from_bytes(&qx, pub + 1, 32) < 0 ||
        bn_from_bytes(&qy, pub + 33, 32) < 0)
        return 0;
    fp_enter(&q.x, &qx);
    fp_enter(&q.y, &qy);
    q.z = p256_one_mont;
    q.infinity = 0;

    p256_mul(&t1, &u1, &g);
    p256_mul(&t2, &u2, &q);
    p256_add(&sum, &t1, &t2);

    if (sum.infinity)
        return 0;

    /* x afiniczny = X / Z^2, potem wyjscie z dziedziny Montgomery'ego */
    struct bignum zinv, zinv2, x_aff;
    fp_inv(&zinv, &sum.z);
    fp_mul(&zinv2, &zinv, &zinv);
    fp_mul(&x_aff, &sum.x, &zinv2);
    fp_leave(&x_aff, &x_aff);

    if (bn_cmp(&x_aff, &p256_n) >= 0)
        bn_sub(&x_aff, &p256_n);

    return bn_cmp(&x_aff, &r) == 0;
}

/* Podpis w certyfikacie jest w DER: SEQUENCE { INTEGER r, INTEGER s } */
static inline int ecdsa_verify_der(const uint8_t *pub, uint32_t publen,
                                   const uint8_t *hash, const uint8_t *der,
                                   uint32_t derlen)
{
    if (derlen < 8 || der[0] != 0x30)
        return 0;

    uint32_t i = 1;
    uint32_t seqlen = der[i++];
    if (seqlen & 0x80) {
        uint32_t nb = seqlen & 0x7f;
        if (nb != 1 || i >= derlen)
            return 0;
        seqlen = der[i++];
    }
    if (i + seqlen > derlen)
        return 0;

    if (der[i] != 0x02)
        return 0;
    i++;
    uint32_t rlen = der[i++];
    const uint8_t *r = der + i;
    i += rlen;
    if (i + 2 > derlen || der[i] != 0x02)
        return 0;
    i++;
    uint32_t slen = der[i++];
    const uint8_t *s = der + i;
    if (i + slen > derlen)
        return 0;

    return ecdsa_p256_verify(pub, publen, hash, r, rlen, s, slen);
}

#endif
