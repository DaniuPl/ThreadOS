/* ECDSA na krzywych NIST P-256 i P-384 (sprawdzanie podpisow)
 *
 * Certyfikaty na krzywej eliptycznej sa dzis czestsze niz RSA, a czesc
 * urzedow (np. ISRG Root X2) podpisuje na P-384 skrotem SHA-384.
 *
 * Arytmetyka stoi na tych samych wielkich liczbach co RSA: mnozenie
 * Montgomery'ego modulo p. Wspolrzedne punktow trzymamy w postaci
 * Jacobiego (x = X/Z^2, y = Y/Z^3), dzieki czemu dodawanie punktow nie
 * wymaga odwrotnosci - ta liczy sie raz, na samym koncu.
 *
 * Obie krzywe maja a = -3, wiec jeden komplet wzorow obsluguje je
 * razem; rozni je tylko modulus, rzad grupy i punkt bazowy.
 *
 * To weryfikacja publicznych danych, wiec kod nie musi byc odporny na
 * pomiar czasu - i nie jest.
 */

#ifndef PROTONOS_UECDSA_H
#define PROTONOS_UECDSA_H

#include "uapi.h"
#include "ubignum.h"

/* Krzywa NIST P256 (256 bitow) */
static const uint8_t p256_p_bytes[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,
};
static const uint8_t p256_n_bytes[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,
    0xca,0xc2,0xfc,0x63,0x25,0x51,
};
static const uint8_t p256_gx_bytes[32] = {
    0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,
    0xa4,0x40,0xf2,0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,
    0x39,0x45,0xd8,0x98,0xc2,0x96,
};
static const uint8_t p256_gy_bytes[32] = {
    0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,
    0x0f,0x9e,0x16,0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,
    0x40,0x68,0x37,0xbf,0x51,0xf5,
};

/* Krzywa NIST P384 (384 bitow) */
static const uint8_t p384_p_bytes[48] = {
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xfe,0xff,0xff,0xff,0xff,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,
};
static const uint8_t p384_n_bytes[48] = {
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xc7,0x63,
    0x4d,0x81,0xf4,0x37,0x2d,0xdf,0x58,0x1a,0x0d,0xb2,0x48,0xb0,0xa7,
    0x7a,0xec,0xec,0x19,0x6a,0xcc,0xc5,0x29,0x73,
};
static const uint8_t p384_gx_bytes[48] = {
    0xaa,0x87,0xca,0x22,0xbe,0x8b,0x05,0x37,0x8e,0xb1,0xc7,0x1e,0xf3,
    0x20,0xad,0x74,0x6e,0x1d,0x3b,0x62,0x8b,0xa7,0x9b,0x98,0x59,0xf7,
    0x41,0xe0,0x82,0x54,0x2a,0x38,0x55,0x02,0xf2,0x5d,0xbf,0x55,0x29,
    0x6c,0x3a,0x54,0x5e,0x38,0x72,0x76,0x0a,0xb7,
};
static const uint8_t p384_gy_bytes[48] = {
    0x36,0x17,0xde,0x4a,0x96,0x26,0x2c,0x6f,0x5d,0x9e,0x98,0xbf,0x92,
    0x92,0xdc,0x29,0xf8,0xf4,0x1d,0xbd,0x28,0x9a,0x14,0x7c,0xe9,0xda,
    0x31,0x13,0xb5,0xf0,0xb8,0xc0,0x0a,0x60,0xb1,0xce,0x1d,0x7e,0x81,
    0x9d,0x7a,0x43,0x1d,0x7c,0x90,0xea,0x0e,0x5f,
};

/* Komplet parametrow krzywej; liczby robocze wyliczaja sie przy
   pierwszym uzyciu (ec_init) */
struct ec_curve {
    uint32_t size;                       /* dlugosc wspolrzednej w bajtach */
    const uint8_t *p_bytes, *n_bytes, *gx_bytes, *gy_bytes;
    struct bignum p, n, r2p, r2n, one_mont;
    uint32_t pinv, ninv;
    int ready;
};

static struct ec_curve ec_p256 = {
    32, p256_p_bytes, p256_n_bytes, p256_gx_bytes, p256_gy_bytes,
    {{0}, 0}, {{0}, 0}, {{0}, 0}, {{0}, 0}, {{0}, 0}, 0, 0, 0
};

static struct ec_curve ec_p384 = {
    48, p384_p_bytes, p384_n_bytes, p384_gx_bytes, p384_gy_bytes,
    {{0}, 0}, {{0}, 0}, {{0}, 0}, {{0}, 0}, {{0}, 0}, 0, 0, 0
};

/* Krzywe wystawiamy funkcjami, a nie wprost.
 *
 * Powod jest w bibliotece wspoldzielonej: to sa DANE, i to zmienne -
 * krzywa doliczaja sobie stale Montgomery'ego przy pierwszym uzyciu.
 * Program, ktory siegalby po nie po nazwie, dostalby wlasna kopie
 * (relokacja kopiujaca), i od tej chwili biblioteka liczylaby na
 * jednym egzemplarzu, a program patrzyl na drugi. Przez funkcje
 * wszyscy widza ten sam. */
static inline struct ec_curve *ec_curve_p256(void) { return &ec_p256; }
static inline struct ec_curve *ec_curve_p384(void) { return &ec_p384; }

/* Punkt w postaci Jacobiego, wspolrzedne w dziedzinie Montgomery'ego */
struct ec_point {
    struct bignum x, y, z;
    int infinity;
};

/* R^2 mod m - brama do dziedziny Montgomery'ego */
static inline void ec_calc_r2(struct bignum *r2, const struct bignum *m)
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

static inline void ec_init(struct ec_curve *c)
{
    if (c->ready)
        return;

    bn_from_bytes(&c->p, c->p_bytes, c->size);
    bn_from_bytes(&c->n, c->n_bytes, c->size);
    c->pinv = bn_inv32(c->p.n[0]);
    c->ninv = bn_inv32(c->n.n[0]);
    ec_calc_r2(&c->r2p, &c->p);
    ec_calc_r2(&c->r2n, &c->n);

    struct bignum one;
    bn_zero(&one);
    one.len = 1;
    one.n[0] = 1;
    bn_mont_mul(&c->one_mont, &one, &c->r2p, &c->p, c->pinv);

    c->ready = 1;
}

/* --- cialo modulo p, wartosci w dziedzinie Montgomery'ego --- */

static inline void fp_mul(const struct ec_curve *c, struct bignum *o,
                          const struct bignum *a, const struct bignum *b)
{
    bn_mont_mul(o, a, b, &c->p, c->pinv);
}

static inline void fp_add(const struct ec_curve *c, struct bignum *o,
                          const struct bignum *a, const struct bignum *b)
{
    bn_add_mod(o, a, b, &c->p);
}

static inline void fp_sub(const struct ec_curve *c, struct bignum *o,
                          const struct bignum *a, const struct bignum *b)
{
    bn_sub_mod(o, a, b, &c->p);
}

static inline void fp_enter(const struct ec_curve *c, struct bignum *o,
                            const struct bignum *a)
{
    bn_mont_mul(o, a, &c->r2p, &c->p, c->pinv);
}

static inline void fp_leave(const struct ec_curve *c, struct bignum *o,
                            const struct bignum *a)
{
    struct bignum one;
    bn_zero(&one);
    one.len = 1;
    one.n[0] = 1;
    bn_mont_mul(o, a, &one, &c->p, c->pinv);
}

/* Odwrotnosc przez male twierdzenie Fermata: a^(p-2).
   Obie krzywe maja p konczace sie na ...ff, wiec p-2 to odjecie
   dwoch od ostatniego bajtu. */
static inline void fp_inv(const struct ec_curve *c, struct bignum *o,
                          const struct bignum *a)
{
    uint8_t e[64];

    if (c->size == 0 || c->size > sizeof(e)) {   /* parametry z kosmosu */
        bn_zero(o);
        return;
    }
    for (uint32_t i = 0; i < c->size; i++)
        e[i] = c->p_bytes[i];
    e[c->size - 1] -= 2;

    struct bignum acc = c->one_mont;
    struct bignum base = *a;
    int started = 0;

    for (uint32_t i = 0; i < c->size; i++) {
        for (int b = 7; b >= 0; b--) {
            if (started)
                fp_mul(c, &acc, &acc, &acc);
            if ((e[i] >> b) & 1) {
                if (started) {
                    fp_mul(c, &acc, &acc, &base);
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

static inline void ec_double(const struct ec_curve *c, struct ec_point *r,
                             const struct ec_point *p)
{
    struct bignum delta, gamma, beta, alpha, t1, t2;

    if (p->infinity || bn_is_zero(&p->y)) {
        r->infinity = 1;
        return;
    }

    fp_mul(c, &delta, &p->z, &p->z);     /* delta = Z^2 */
    fp_mul(c, &gamma, &p->y, &p->y);     /* gamma = Y^2 */
    fp_mul(c, &beta, &p->x, &gamma);     /* beta = X*gamma */

    fp_sub(c, &t1, &p->x, &delta);       /* alpha = 3*(X-delta)*(X+delta) */
    fp_add(c, &t2, &p->x, &delta);
    fp_mul(c, &alpha, &t1, &t2);
    fp_add(c, &t1, &alpha, &alpha);
    fp_add(c, &alpha, &t1, &alpha);

    fp_add(c, &t1, &p->y, &p->z);        /* Z3 = (Y+Z)^2 - gamma - delta */
    fp_mul(c, &t1, &t1, &t1);
    fp_sub(c, &t1, &t1, &gamma);
    fp_sub(c, &r->z, &t1, &delta);

    fp_mul(c, &t1, &alpha, &alpha);      /* X3 = alpha^2 - 8*beta */
    fp_add(c, &t2, &beta, &beta);
    fp_add(c, &t2, &t2, &t2);
    fp_add(c, &t2, &t2, &t2);
    fp_sub(c, &t1, &t1, &t2);

    struct bignum x3 = t1;

    fp_add(c, &t2, &beta, &beta);        /* Y3 = alpha*(4*beta - X3) */
    fp_add(c, &t2, &t2, &t2);            /*      - 8*gamma^2         */
    fp_sub(c, &t2, &t2, &x3);
    fp_mul(c, &t2, &alpha, &t2);

    fp_mul(c, &t1, &gamma, &gamma);
    fp_add(c, &t1, &t1, &t1);
    fp_add(c, &t1, &t1, &t1);
    fp_add(c, &t1, &t1, &t1);
    fp_sub(c, &r->y, &t2, &t1);

    r->x = x3;
    r->infinity = 0;
}

static inline void ec_add(const struct ec_curve *c, struct ec_point *r,
                          const struct ec_point *p, const struct ec_point *q)
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

    fp_mul(c, &z1z1, &p->z, &p->z);
    fp_mul(c, &z2z2, &q->z, &q->z);
    fp_mul(c, &u1, &p->x, &z2z2);
    fp_mul(c, &u2, &q->x, &z1z1);

    fp_mul(c, &t1, &q->z, &z2z2);
    fp_mul(c, &s1, &p->y, &t1);
    fp_mul(c, &t2, &p->z, &z1z1);
    fp_mul(c, &s2, &q->y, &t2);

    fp_sub(c, &h, &u2, &u1);
    fp_sub(c, &rr, &s2, &s1);

    if (bn_is_zero(&h)) {
        if (bn_is_zero(&rr)) {
            ec_double(c, r, p);          /* ten sam punkt */
            return;
        }
        r->infinity = 1;                 /* punkty przeciwne */
        return;
    }

    fp_add(c, &rr, &rr, &rr);            /* r = 2*(S2-S1) */

    fp_add(c, &t1, &h, &h);              /* I = (2H)^2 */
    fp_mul(c, &i, &t1, &t1);
    fp_mul(c, &j, &h, &i);               /* J = H*I */
    fp_mul(c, &v, &u1, &i);              /* V = U1*I */

    fp_mul(c, &t1, &rr, &rr);            /* X3 = r^2 - J - 2V */
    fp_sub(c, &t1, &t1, &j);
    fp_add(c, &t2, &v, &v);
    fp_sub(c, &t1, &t1, &t2);
    struct bignum x3 = t1;

    fp_sub(c, &t1, &v, &x3);             /* Y3 = r*(V-X3) - 2*S1*J */
    fp_mul(c, &t1, &rr, &t1);
    fp_mul(c, &t2, &s1, &j);
    fp_add(c, &t2, &t2, &t2);
    fp_sub(c, &r->y, &t1, &t2);

    fp_add(c, &t1, &p->z, &q->z);        /* Z3 = ((Z1+Z2)^2-Z1Z1-Z2Z2)*H */
    fp_mul(c, &t1, &t1, &t1);
    fp_sub(c, &t1, &t1, &z1z1);
    fp_sub(c, &t1, &t1, &z2z2);
    fp_mul(c, &r->z, &t1, &h);

    r->x = x3;
    r->infinity = 0;
}

/* r = k*p (podwajaj i dodawaj, od najstarszego bitu) */
static inline void ec_mul(const struct ec_curve *c, struct ec_point *r,
                          const struct bignum *k, const struct ec_point *p)
{
    struct ec_point acc;
    acc.infinity = 1;
    bn_zero(&acc.x);
    bn_zero(&acc.y);
    bn_zero(&acc.z);

    int started = 0;
    for (int limb = k->len - 1; limb >= 0; limb--) {
        for (int b = 31; b >= 0; b--) {
            if (started)
                ec_double(c, &acc, &acc);
            if ((k->n[limb] >> b) & 1) {
                if (started) {
                    ec_add(c, &acc, &acc, p);
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
static inline void mod_n_mul(const struct ec_curve *c, struct bignum *o,
                             const struct bignum *a, const struct bignum *b)
{
    struct bignum t;
    bn_mont_mul(&t, a, b, &c->n, c->ninv);
    bn_mont_mul(o, &t, &c->r2n, &c->n, c->ninv);
}

/* Sprawdza podpis (r,s) pod skrotem dla klucza publicznego podanego
   jako punkt nieskompresowany: 0x04 || X || Y. 1 = podpis dobry. */
static inline int ecdsa_verify(struct ec_curve *c, const uint8_t *pub,
                               uint32_t publen, const uint8_t *hash,
                               uint32_t hashlen, const uint8_t *r_bytes,
                               uint32_t rlen, const uint8_t *s_bytes,
                               uint32_t slen)
{
    ec_init(c);

    if (publen != 1 + 2 * c->size || pub[0] != 0x04)
        return 0;                        /* tylko postac nieskompresowana */

    struct bignum r, s, e, w, u1, u2, qx, qy;
    if (bn_from_bytes(&r, r_bytes, rlen) < 0 ||
        bn_from_bytes(&s, s_bytes, slen) < 0)
        return 0;
    if (bn_is_zero(&r) || bn_is_zero(&s))
        return 0;
    if (bn_cmp(&r, &c->n) >= 0 || bn_cmp(&s, &c->n) >= 0)
        return 0;

    /* Z dluzszego skrotu bierzemy tyle najstarszych bajtow, ile ma rzad */
    uint32_t use = hashlen < c->size ? hashlen : c->size;
    bn_from_bytes(&e, hash, use);
    if (bn_cmp(&e, &c->n) >= 0)
        bn_sub(&e, &c->n);

    /* w = s^-1 mod n */
    uint8_t nm2[64];
    if (c->size == 0 || c->size > sizeof(nm2))
        return 0;
    for (uint32_t i = 0; i < c->size; i++)
        nm2[i] = c->n_bytes[i];
    nm2[c->size - 1] -= 2;
    if (bn_modexp_bytes(&w, &s, nm2, c->size, &c->n) < 0)
        return 0;

    mod_n_mul(c, &u1, &e, &w);
    mod_n_mul(c, &u2, &r, &w);

    /* punkty: G i klucz publiczny */
    struct ec_point g, q, t1, t2, sum;
    struct bignum tmp;

    bn_from_bytes(&tmp, c->gx_bytes, c->size);
    fp_enter(c, &g.x, &tmp);
    bn_from_bytes(&tmp, c->gy_bytes, c->size);
    fp_enter(c, &g.y, &tmp);
    g.z = c->one_mont;
    g.infinity = 0;

    if (bn_from_bytes(&qx, pub + 1, c->size) < 0 ||
        bn_from_bytes(&qy, pub + 1 + c->size, c->size) < 0)
        return 0;
    fp_enter(c, &q.x, &qx);
    fp_enter(c, &q.y, &qy);
    q.z = c->one_mont;
    q.infinity = 0;

    ec_mul(c, &t1, &u1, &g);
    ec_mul(c, &t2, &u2, &q);
    ec_add(c, &sum, &t1, &t2);

    if (sum.infinity)
        return 0;

    /* x afiniczny = X / Z^2, potem wyjscie z dziedziny Montgomery'ego */
    struct bignum zinv, zinv2, x_aff;
    fp_inv(c, &zinv, &sum.z);
    fp_mul(c, &zinv2, &zinv, &zinv);
    fp_mul(c, &x_aff, &sum.x, &zinv2);
    fp_leave(c, &x_aff, &x_aff);

    if (bn_cmp(&x_aff, &c->n) >= 0)
        bn_sub(&x_aff, &c->n);

    return bn_cmp(&x_aff, &r) == 0;
}

/* Podpis w certyfikacie jest w DER: SEQUENCE { INTEGER r, INTEGER s } */
static inline int ecdsa_verify_der(struct ec_curve *c, const uint8_t *pub,
                                   uint32_t publen, const uint8_t *hash,
                                   uint32_t hashlen, const uint8_t *der,
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

    return ecdsa_verify(c, pub, publen, hash, hashlen, r, rlen, s, slen);
}

#endif
