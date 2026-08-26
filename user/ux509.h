/* Parser certyfikatow X.509 (DER) - tyle, ile trzeba do TLS
 *
 * Certyfikat to zagniezdzone struktury w zapisie DER: kazdy element ma
 * znacznik, dlugosc i tresc. Wyciagamy z niego: bajty podpisane przez
 * wystawce (tbsCertificate), sam podpis i jego algorytm, klucz publiczny,
 * daty waznosci, nazwy wystawcy i podmiotu (do sklejenia lancucha),
 * SubjectAltName (do sprawdzenia nazwy hosta) i flage CA.
 *
 * Nie kopiujemy niczego - pola wskazuja w oryginalny bufor certyfikatu.
 */

#ifndef PROTONOS_UX509_H
#define PROTONOS_UX509_H

#include "usys.h"

#define X509_ALG_UNKNOWN     0
#define X509_ALG_RSA_SHA256  1
#define X509_ALG_RSA_PSS     2
#define X509_ALG_ECDSA_SHA256 3

#define X509_KEY_UNKNOWN 0
#define X509_KEY_RSA     1
#define X509_KEY_EC      2

struct x509_cert {
    const uint8_t *tbs;      uint32_t tbs_len;
    const uint8_t *sig;      uint32_t sig_len;
    int sig_alg;

    const uint8_t *issuer;   uint32_t issuer_len;
    const uint8_t *subject;  uint32_t subject_len;

    int key_type;
    const uint8_t *mod;      uint32_t mod_len;    /* RSA: modulus */
    uint32_t exp;                                 /* RSA: wykladnik */
    const uint8_t *ec_point; uint32_t ec_point_len; /* EC: 0x04||X||Y */
    int ec_p256;                                  /* czy krzywa P-256 */

    char not_before[16], not_after[16];           /* YYYYMMDDHHMMSS */

    const uint8_t *san;      uint32_t san_len;    /* SubjectAltName */
    int is_ca;
};

/* --- podstawy DER --- */

/* Czyta jeden element: znacznik, tresc i dlugosc; przesuwa *p dalej */
static inline int der_next(const uint8_t **p, const uint8_t *end,
                           uint8_t *tag, const uint8_t **content,
                           uint32_t *clen)
{
    const uint8_t *q = *p;
    if (q + 2 > end)
        return -1;

    *tag = *q++;
    uint32_t len = *q++;
    if (len & 0x80) {                    /* dluga postac dlugosci */
        uint32_t nbytes = len & 0x7f;
        if (nbytes == 0 || nbytes > 4 || q + nbytes > end)
            return -1;
        len = 0;
        while (nbytes--)
            len = (len << 8) | *q++;
    }
    if (q + len > end)
        return -1;

    *content = q;
    *clen = len;
    *p = q + len;
    return 0;
}

/* Wchodzi do wnetrza elementu (SEQUENCE, SET, [n] EXPLICIT) */
static inline int der_enter(const uint8_t **p, const uint8_t *end,
                            uint8_t want_tag, const uint8_t **inner,
                            const uint8_t **inner_end)
{
    uint8_t tag;
    const uint8_t *c;
    uint32_t len;
    if (der_next(p, end, &tag, &c, &len) < 0)
        return -1;
    if (want_tag && tag != want_tag)
        return -1;
    *inner = c;
    *inner_end = c + len;
    return 0;
}

static inline int der_oid_is(const uint8_t *oid, uint32_t len,
                             const uint8_t *want, uint32_t wantlen)
{
    if (len != wantlen)
        return 0;
    for (uint32_t i = 0; i < len; i++)
        if (oid[i] != want[i])
            return 0;
    return 1;
}

/* Rozpoznanie AlgorithmIdentifier po identyfikatorze obiektu */
static inline int x509_alg_from_oid(const uint8_t *oid, uint32_t len)
{
    static const uint8_t rsa_sha256[] =
        {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b};
    static const uint8_t rsa_pss[] =
        {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0a};
    static const uint8_t ecdsa_sha256[] =
        {0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02};

    if (der_oid_is(oid, len, rsa_sha256, sizeof(rsa_sha256)))
        return X509_ALG_RSA_SHA256;
    if (der_oid_is(oid, len, rsa_pss, sizeof(rsa_pss)))
        return X509_ALG_RSA_PSS;
    if (der_oid_is(oid, len, ecdsa_sha256, sizeof(ecdsa_sha256)))
        return X509_ALG_ECDSA_SHA256;
    return X509_ALG_UNKNOWN;
}

/* Czas: UTCTime (YYMMDD...) albo GeneralizedTime (YYYYMMDD...) */
static inline void x509_time(uint8_t tag, const uint8_t *p, uint32_t len,
                             char *out)
{
    int n = 0;
    if (tag == 0x17 && len >= 12) {      /* UTCTime: rok dwucyfrowy */
        int yy = (p[0] - '0') * 10 + (p[1] - '0');
        const char *century = (yy >= 50) ? "19" : "20";
        out[n++] = century[0];
        out[n++] = century[1];
        for (uint32_t i = 0; i < 12 && n < 15; i++)
            out[n++] = (char)p[i];
    } else if (tag == 0x18 && len >= 14) {
        for (uint32_t i = 0; i < 14 && n < 15; i++)
            out[n++] = (char)p[i];
    }
    out[n] = '\0';
}

/* --- rozbior certyfikatu --- */

static inline int x509_parse(struct x509_cert *c, const uint8_t *der,
                             uint32_t len)
{
    const uint8_t *p = der, *end = der + len;
    const uint8_t *cert, *cert_end;

    for (int i = 0; i < (int)sizeof(*c); i++)
        ((uint8_t *)c)[i] = 0;

    if (der_enter(&p, end, 0x30, &cert, &cert_end) < 0)
        return -1;

    /* tbsCertificate - zapamietujemy caly element razem z naglowkiem,
       bo to jego bajty podpisuje wystawca */
    const uint8_t *tbs_start = cert;
    const uint8_t *tbs, *tbs_end;
    const uint8_t *q = cert;
    if (der_enter(&q, cert_end, 0x30, &tbs, &tbs_end) < 0)
        return -1;
    c->tbs = tbs_start;
    c->tbs_len = (uint32_t)(tbs_end - tbs_start);

    /* signatureAlgorithm */
    const uint8_t *alg, *alg_end;
    if (der_enter(&q, cert_end, 0x30, &alg, &alg_end) < 0)
        return -1;
    uint8_t tag;
    const uint8_t *oid;
    uint32_t oidlen;
    if (der_next(&alg, alg_end, &tag, &oid, &oidlen) < 0 || tag != 0x06)
        return -1;
    c->sig_alg = x509_alg_from_oid(oid, oidlen);

    /* signatureValue (BIT STRING - pierwszy bajt to liczba nieuzywanych bitow) */
    const uint8_t *sig;
    uint32_t siglen;
    if (der_next(&q, cert_end, &tag, &sig, &siglen) < 0 || tag != 0x03 ||
        siglen < 2)
        return -1;
    c->sig = sig + 1;
    c->sig_len = siglen - 1;

    /* --- wnetrze tbsCertificate --- */
    const uint8_t *t = tbs;
    const uint8_t *content;
    uint32_t clen;

    if (t < tbs_end && *t == 0xa0) {     /* [0] version */
        if (der_next(&t, tbs_end, &tag, &content, &clen) < 0)
            return -1;
    }
    if (der_next(&t, tbs_end, &tag, &content, &clen) < 0)   /* serialNumber */
        return -1;
    if (der_next(&t, tbs_end, &tag, &content, &clen) < 0)   /* signature */
        return -1;

    const uint8_t *issuer_start = t;                        /* issuer Name */
    if (der_next(&t, tbs_end, &tag, &content, &clen) < 0 || tag != 0x30)
        return -1;
    c->issuer = issuer_start;
    c->issuer_len = (uint32_t)(t - issuer_start);

    /* validity */
    const uint8_t *val, *val_end;
    if (der_enter(&t, tbs_end, 0x30, &val, &val_end) < 0)
        return -1;
    if (der_next(&val, val_end, &tag, &content, &clen) < 0)
        return -1;
    x509_time(tag, content, clen, c->not_before);
    if (der_next(&val, val_end, &tag, &content, &clen) < 0)
        return -1;
    x509_time(tag, content, clen, c->not_after);

    const uint8_t *subject_start = t;                       /* subject Name */
    if (der_next(&t, tbs_end, &tag, &content, &clen) < 0 || tag != 0x30)
        return -1;
    c->subject = subject_start;
    c->subject_len = (uint32_t)(t - subject_start);

    /* subjectPublicKeyInfo */
    const uint8_t *spki, *spki_end;
    if (der_enter(&t, tbs_end, 0x30, &spki, &spki_end) < 0)
        return -1;
    const uint8_t *kalg, *kalg_end;
    if (der_enter(&spki, spki_end, 0x30, &kalg, &kalg_end) < 0)
        return -1;
    if (der_next(&kalg, kalg_end, &tag, &oid, &oidlen) < 0 || tag != 0x06)
        return -1;

    /* Dla krzywych po identyfikatorze algorytmu idzie identyfikator
       krzywej - interesuje nas tylko prime256v1 (NIST P-256) */
    if (kalg < kalg_end) {
        const uint8_t *curve;
        uint32_t curvelen;
        uint8_t ctag;
        static const uint8_t oid_p256[] =
            {0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07};
        if (der_next(&kalg, kalg_end, &ctag, &curve, &curvelen) == 0 &&
            ctag == 0x06 &&
            der_oid_is(curve, curvelen, oid_p256, sizeof(oid_p256)))
            c->ec_p256 = 1;
    }

    static const uint8_t oid_rsa[] =
        {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
    static const uint8_t oid_ec[] = {0x2a,0x86,0x48,0xce,0x3d,0x02,0x01};
    if (der_oid_is(oid, oidlen, oid_rsa, sizeof(oid_rsa)))
        c->key_type = X509_KEY_RSA;
    else if (der_oid_is(oid, oidlen, oid_ec, sizeof(oid_ec)))
        c->key_type = X509_KEY_EC;

    const uint8_t *bits;
    uint32_t bitlen;
    if (der_next(&spki, spki_end, &tag, &bits, &bitlen) < 0 || tag != 0x03 ||
        bitlen < 2)
        return -1;

    if (c->key_type == X509_KEY_EC) {    /* punkt: 0x04 || X || Y */
        c->ec_point = bits + 1;
        c->ec_point_len = bitlen - 1;
    }

    if (c->key_type == X509_KEY_RSA) {   /* SEQUENCE { modulus, exponent } */
        const uint8_t *k = bits + 1, *k_end = bits + bitlen;
        const uint8_t *rsa, *rsa_end;
        if (der_enter(&k, k_end, 0x30, &rsa, &rsa_end) < 0)
            return -1;
        if (der_next(&rsa, rsa_end, &tag, &content, &clen) < 0 || tag != 0x02)
            return -1;
        while (clen > 0 && *content == 0) {   /* wiodace zero znaku */
            content++;
            clen--;
        }
        c->mod = content;
        c->mod_len = clen;
        if (der_next(&rsa, rsa_end, &tag, &content, &clen) < 0 || tag != 0x02)
            return -1;
        uint32_t e = 0;
        for (uint32_t i = 0; i < clen && i < 4; i++)
            e = (e << 8) | content[i];
        c->exp = e;
    }

    /* rozszerzenia: [3] EXPLICIT SEQUENCE OF Extension */
    while (t < tbs_end) {
        const uint8_t *saved = t;
        if (der_next(&t, tbs_end, &tag, &content, &clen) < 0)
            break;
        if (tag != 0xa3) {
            (void)saved;
            continue;
        }

        const uint8_t *exts = content, *exts_end = content + clen;
        const uint8_t *list, *list_end;
        if (der_enter(&exts, exts_end, 0x30, &list, &list_end) < 0)
            break;

        while (list < list_end) {
            const uint8_t *ext, *ext_end;
            if (der_enter(&list, list_end, 0x30, &ext, &ext_end) < 0)
                break;
            if (der_next(&ext, ext_end, &tag, &oid, &oidlen) < 0 || tag != 0x06)
                break;
            if (ext < ext_end && *ext == 0x01)      /* critical BOOLEAN */
                der_next(&ext, ext_end, &tag, &content, &clen);

            const uint8_t *val_oct;
            uint32_t val_len;
            if (der_next(&ext, ext_end, &tag, &val_oct, &val_len) < 0 ||
                tag != 0x04)
                break;

            static const uint8_t oid_san[] = {0x55, 0x1d, 0x11};
            static const uint8_t oid_bc[] = {0x55, 0x1d, 0x13};

            if (der_oid_is(oid, oidlen, oid_san, sizeof(oid_san))) {
                c->san = val_oct;
                c->san_len = val_len;
            } else if (der_oid_is(oid, oidlen, oid_bc, sizeof(oid_bc))) {
                const uint8_t *bc = val_oct, *bc_end = val_oct + val_len;
                const uint8_t *seq, *seq_end;
                if (der_enter(&bc, bc_end, 0x30, &seq, &seq_end) == 0) {
                    if (seq < seq_end && *seq == 0x01) {   /* cA BOOLEAN */
                        const uint8_t *b;
                        uint32_t bl;
                        if (der_next(&seq, seq_end, &tag, &b, &bl) == 0 &&
                            bl == 1 && b[0] != 0)
                            c->is_ca = 1;
                    }
                }
            }
        }
    }

    return 0;
}

/* --- dopasowanie nazwy hosta --- */

static inline int x509_name_eq(const char *pattern, uint32_t plen,
                               const char *host)
{
    /* wieloznacznik tylko na poczatku: *.przyklad.pl */
    if (plen > 2 && pattern[0] == '*' && pattern[1] == '.') {
        const char *dot = host;
        while (*dot && *dot != '.')
            dot++;
        if (!*dot)
            return 0;
        pattern += 1;                    /* zostaw kropke */
        plen -= 1;
        host = dot;
    }

    uint32_t i = 0;
    for (; i < plen && host[i]; i++) {
        char a = pattern[i], b = host[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b)
            return 0;
    }
    return i == plen && !host[i];
}

/* Czy certyfikat obejmuje dana nazwe? Sprawdzamy SubjectAltName. */
static inline int x509_matches_host(const struct x509_cert *c, const char *host)
{
    if (!c->san)
        return 0;

    const uint8_t *p = c->san, *end = c->san + c->san_len;
    const uint8_t *names, *names_end;
    if (der_enter(&p, end, 0x30, &names, &names_end) < 0)
        return 0;

    while (names < names_end) {
        uint8_t tag;
        const uint8_t *content;
        uint32_t clen;
        if (der_next(&names, names_end, &tag, &content, &clen) < 0)
            break;
        if (tag == 0x82 &&               /* [2] dNSName */
            x509_name_eq((const char *)content, clen, host))
            return 1;
    }
    return 0;
}

/* Porownanie nazw wyroznionych (bajt w bajt - tak robi wiekszosc) */
static inline int x509_name_match(const uint8_t *a, uint32_t alen,
                                  const uint8_t *b, uint32_t blen)
{
    if (alen != blen)
        return 0;
    for (uint32_t i = 0; i < alen; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

#endif
