/* Certificate chain verification
 *
 * An encrypted channel by itself gives nothing if it is not known who
 * is on the other side. Here we check three things:
 *   1. does the certificate cover the name we connected to,
 *   2. is it valid according to the system clock,
 *   3. does it lead a chain of signatures to a certificate we trust.
 *
 * A trust store is a concatenation of certificates in DER. Some of
 * them are built into the system (caroots.h - the roots of public CAs),
 * and more can be added with a "roots.der" file on disk - useful for
 * one's own certificates on a local network.
 */

#ifndef THREADOS_UCERT_H
#define THREADOS_UCERT_H

#include "uapi.h"
#include "usha256.h"
#include "ursa.h"
#include "ux509.h"
#include "uecdsa.h"
#include "usha512.h"
#include "caroots.h"

/* We expose the built-in trust store through a function, not an array.
   Just like the curves in uecdsa.h: these are data, and data sit inside
   the shared library and are to stay there - a program gets a pointer
   to them rather than its own copy of a dozen or so kilobytes of
   certificates. */
static inline const uint8_t *cert_builtin_roots(uint32_t *count)
{
    *count = (uint32_t)sizeof(ca_roots_der);
    return ca_roots_der;
}

#define CERT_CHAIN_MAX 5
#define CERT_ROOTS_MAX 8

#define CERT_OK              0
#define CERT_ERR_NAME        1
#define CERT_ERR_EXPIRED     2
#define CERT_ERR_NOT_YET     3
#define CERT_ERR_UNTRUSTED   4
#define CERT_ERR_SIGNATURE   5
#define CERT_ERR_ALG         6
#define CERT_ERR_PARSE       7

static inline const char *cert_error_text(int err)
{
    switch (err) {
    case CERT_OK:            return "in order";
    case CERT_ERR_NAME:      return "the certificate does not cover this name";
    case CERT_ERR_EXPIRED:   return "the certificate has expired";
    case CERT_ERR_NOT_YET:   return "the certificate is not valid yet";
    case CERT_ERR_UNTRUSTED: return "no trusted issuer";
    case CERT_ERR_SIGNATURE: return "the signature does not match";
    case CERT_ERR_ALG:       return "an unsupported signature algorithm";
    default:                 return "the certificate cannot be read";
    }
}

/* The current time from the RTC in the form YYYYMMDDHHMMSS */
static inline void cert_now(char *out)
{
    struct utime t;
    sys_time(&t);

    uint32_t v[6] = { t.year, t.month, t.day, t.hour, t.min, t.sec };
    int widths[6] = { 4, 2, 2, 2, 2, 2 };
    int n = 0;

    for (int i = 0; i < 6; i++) {
        for (int d = widths[i] - 1; d >= 0; d--) {
            uint32_t div = 1;
            for (int k = 0; k < d; k++)
                div *= 10;
            out[n++] = (char)('0' + (v[i] / div) % 10);
        }
    }
    out[n] = '\0';
}

static inline int cert_str_cmp(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
        i++;
    }
    if (a[i] == b[i])
        return 0;
    return a[i] ? 1 : -1;
}

/* Was the certificate 'sub' signed with the key from 'issuer'? */
static inline int cert_signed_by(const struct x509_cert *sub,
                                 const struct x509_cert *issuer)
{
    uint8_t hash[64];

    if (!x509_name_match(sub->issuer, sub->issuer_len,
                         issuer->subject, issuer->subject_len))
        return 0;

    if (issuer->key_type == X509_KEY_EC) {
        struct ec_curve *cv;
        uint32_t hashlen;

        if (issuer->ec_bits == 256)
            cv = &ec_p256;
        else if (issuer->ec_bits == 384)
            cv = &ec_p384;
        else
            return 0;
        if (!issuer->ec_point)
            return 0;

        /* the digest is chosen by the signature algorithm, not by the
   curve */
        if (sub->sig_alg == X509_ALG_ECDSA_SHA256) {
            sha256(sub->tbs, sub->tbs_len, hash);
            hashlen = 32;
        } else if (sub->sig_alg == X509_ALG_ECDSA_SHA384) {
            sha384(sub->tbs, sub->tbs_len, hash);
            hashlen = 48;
        } else {
            return 0;
        }

        return ecdsa_verify_der(cv, issuer->ec_point, issuer->ec_point_len,
                                hash, hashlen, sub->sig, sub->sig_len);
    }

    sha256(sub->tbs, sub->tbs_len, hash);

    if (issuer->key_type != X509_KEY_RSA || !issuer->mod)
        return 0;

    if (sub->sig_alg == X509_ALG_RSA_SHA256)
        return rsa_verify_pkcs1_sha256(issuer->mod, issuer->mod_len,
                                       issuer->exp, sub->sig, sub->sig_len,
                                       hash);
    if (sub->sig_alg == X509_ALG_RSA_PSS)
        return rsa_verify_pss_sha256(issuer->mod, issuer->mod_len, issuer->exp,
                                     sub->sig, sub->sig_len, hash,
                                     issuer->mod_len * 8);
    return 0;                            /* a different algorithm - we do not know it */
}

/* Checks validity against the clock */
static inline int cert_check_dates(const struct x509_cert *c, const char *now)
{
    if (c->not_before[0] && cert_str_cmp(now, c->not_before) < 0)
        return CERT_ERR_NOT_YET;
    if (c->not_after[0] && cert_str_cmp(now, c->not_after) > 0)
        return CERT_ERR_EXPIRED;
    return CERT_OK;
}

/* Did any of the certificates in the store sign 'cur'? The store is
   DER certificates glued together - we walk them, reading the length
   from each one's header. */
static inline int cert_in_store(const struct x509_cert *cur,
                                const uint8_t *store, uint32_t len,
                                const char *now)
{
    const uint8_t *p = store;
    uint32_t left = len;

    while (p && left > 4) {
        struct x509_cert root;
        uint32_t rlen;

        if (p[0] != 0x30)
            break;
        uint32_t l = p[1];
        if (l & 0x80) {
            uint32_t nb = l & 0x7f;
            if (nb > 4 || 2u + nb > left)
                break;
            l = 0;
            for (uint32_t k = 0; k < nb; k++)
                l = (l << 8) | p[2 + k];
            rlen = 2 + nb + l;
        } else {
            rlen = 2 + l;
        }
        if (rlen > left)
            break;

        if (x509_parse(&root, p, rlen) == 0 &&
            cert_check_dates(&root, now) == CERT_OK &&
            cert_signed_by(cur, &root))
            return 1;

        p += rlen;
        left -= rlen;
    }
    return 0;
}

/* The full check: the name, the dates and the chain to a trusted root.
   chain[0] is the server's certificate. */
static inline int cert_verify_chain(struct x509_cert *chain, int n,
                                    const uint8_t *roots, uint32_t roots_len,
                                    const char *host)
{
    char now[16];
    cert_now(now);

    if (n <= 0)
        return CERT_ERR_PARSE;

    if (host && *host && !x509_matches_host(&chain[0], host))
        return CERT_ERR_NAME;

    int err = cert_check_dates(&chain[0], now);
    if (err != CERT_OK)
        return err;

    /* We climb up the chain until we hit somebody trusted */
    for (int i = 0; i < n; i++) {
        struct x509_cert *cur = &chain[i];

        if (cert_in_store(cur, roots, roots_len, now) ||
            cert_in_store(cur, ca_roots_der, sizeof(ca_roots_der), now))
            return CERT_OK;              /* the chain is closed */

        /* if not a root, then maybe the next one in the chain */
        if (i + 1 < n) {
            struct x509_cert *up = &chain[i + 1];
            if (!up->is_ca)
                return CERT_ERR_UNTRUSTED;
            err = cert_check_dates(up, now);
            if (err != CERT_OK)
                return err;
            if (!cert_signed_by(cur, up)) {
                if (cur->sig_alg == X509_ALG_UNKNOWN ||
                    up->key_type == X509_KEY_UNKNOWN)
                    return CERT_ERR_ALG;
                return CERT_ERR_SIGNATURE;
            }
        }
    }

    return CERT_ERR_UNTRUSTED;           /* the chain did not reach a root */
}

/* Reads a trust store from a file on disk. Returns the number of
   bytes. */
static inline uint32_t cert_load_roots(const char *path, uint8_t *buf,
                                       uint32_t max)
{
    int fd = sys_open(path, O_READ);
    if (fd < 0)
        return 0;

    uint32_t total = 0;
    for (;;) {
        int32_t n = sys_read(fd, buf + total, max - total);
        if (n <= 0)
            break;
        total += (uint32_t)n;
        if (total >= max)
            break;
    }
    sys_close(fd);
    return total;
}

#endif
