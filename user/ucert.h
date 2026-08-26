/* Weryfikacja lancucha certyfikatow
 *
 * Sam szyfrowany kanal nic nie daje, jesli nie wiadomo, z kim sie
 * rozmawia. Tu sprawdzamy trzy rzeczy:
 *   1. czy certyfikat obejmuje nazwe, pod ktora sie laczylismy,
 *   2. czy jest wazny wedlug zegara systemu,
 *   3. czy prowadzi lancuch podpisow do certyfikatu, ktoremu ufamy.
 *
 * Magazyn zaufania to konkatenacja certyfikatow w DER. Czesc jest
 * wbudowana w system (caroots.h - korzenie publicznych CA), a dodatkowe
 * mozna dolozyc plikiem "roots.der" na dysku - przydaje sie do wlasnych
 * certyfikatow w sieci lokalnej.
 */

#ifndef PROTONOS_UCERT_H
#define PROTONOS_UCERT_H

#include "usys.h"
#include "usha256.h"
#include "ursa.h"
#include "ux509.h"
#include "uecdsa.h"
#include "caroots.h"

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
    case CERT_OK:            return "w porzadku";
    case CERT_ERR_NAME:      return "certyfikat nie obejmuje tej nazwy";
    case CERT_ERR_EXPIRED:   return "certyfikat wygasl";
    case CERT_ERR_NOT_YET:   return "certyfikat jeszcze nie obowiazuje";
    case CERT_ERR_UNTRUSTED: return "brak zaufanego wystawcy";
    case CERT_ERR_SIGNATURE: return "podpis sie nie zgadza";
    case CERT_ERR_ALG:       return "nieobslugiwany algorytm podpisu";
    default:                 return "certyfikat nie do rozczytania";
    }
}

/* Biezacy czas z RTC w postaci YYYYMMDDHHMMSS */
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

/* Czy certyfikat 'sub' zostal podpisany kluczem z 'issuer'? */
static inline int cert_signed_by(const struct x509_cert *sub,
                                 const struct x509_cert *issuer)
{
    uint8_t hash[32];

    if (!x509_name_match(sub->issuer, sub->issuer_len,
                         issuer->subject, issuer->subject_len))
        return 0;

    sha256(sub->tbs, sub->tbs_len, hash);

    if (issuer->key_type == X509_KEY_EC) {
        if (!issuer->ec_p256 || !issuer->ec_point)
            return 0;
        if (sub->sig_alg != X509_ALG_ECDSA_SHA256)
            return 0;
        return ecdsa_verify_der(issuer->ec_point, issuer->ec_point_len, hash,
                                sub->sig, sub->sig_len);
    }

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
    return 0;                            /* inny algorytm - nie umiemy */
}

/* Sprawdza waznosc wedlug zegara */
static inline int cert_check_dates(const struct x509_cert *c, const char *now)
{
    if (c->not_before[0] && cert_str_cmp(now, c->not_before) < 0)
        return CERT_ERR_NOT_YET;
    if (c->not_after[0] && cert_str_cmp(now, c->not_after) > 0)
        return CERT_ERR_EXPIRED;
    return CERT_OK;
}

/* Czy ktorys z certyfikatow w magazynie podpisal 'cur'? Magazyn to
   sklejone certyfikaty DER - idziemy po nich, czytajac dlugosc z
   naglowka kazdego. */
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

/* Pelne sprawdzenie: nazwa, daty i lancuch do zaufanego korzenia.
   chain[0] to certyfikat serwera. */
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

    /* Wchodzimy w gore lancucha, az trafimy na kogos zaufanego */
    for (int i = 0; i < n; i++) {
        struct x509_cert *cur = &chain[i];

        if (cert_in_store(cur, roots, roots_len, now) ||
            cert_in_store(cur, ca_roots_der, sizeof(ca_roots_der), now))
            return CERT_OK;              /* lancuch domkniety */

        /* jak nie korzen, to moze nastepny w lancuchu */
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

    return CERT_ERR_UNTRUSTED;           /* lancuch nie doszedl do korzenia */
}

/* Wczytuje magazyn zaufania z pliku na dysku. Zwraca liczbe bajtow. */
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
