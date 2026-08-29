/* Sprawdzanie podpisow RSA (RFC 8017): PKCS#1 v1.5 i PSS
 *
 * Do weryfikacji wystarcza klucz publiczny: podnosimy podpis do potegi
 * e modulo n i porownujemy wynik z tym, co powinno tam byc dla naszego
 * skrotu. Certyfikaty CA podpisywane sa zwykle w wersji v1.5, a TLS 1.3
 * wymaga PSS w wiadomosci CertificateVerify - stad obie sciezki.
 */

#ifndef PROTONOS_URSA_H
#define PROTONOS_URSA_H

#include "uapi.h"
#include "usha256.h"
#include "ubignum.h"

#define RSA_MAX_BYTES 512                /* do 4096 bitow */

/* Naglowek DigestInfo dla SHA-256 (staly ciag z RFC 8017) */
static const uint8_t rsa_sha256_prefix[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65,
    0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
};

/* Wspolna czesc: sig^e mod n -> em */
static inline int rsa_public(const uint8_t *mod, uint32_t modlen, uint32_t exp,
                             const uint8_t *sig, uint32_t siglen, uint8_t *em)
{
    struct bignum n, s, m;

    if (siglen != modlen || modlen > RSA_MAX_BYTES)
        return -1;
    if (bn_from_bytes(&n, mod, modlen) < 0)
        return -1;
    if (bn_from_bytes(&s, sig, siglen) < 0)
        return -1;
    if (bn_cmp(&s, &n) >= 0)
        return -1;
    if (bn_modexp(&m, &s, exp, &n) < 0)
        return -1;

    bn_to_bytes(&m, em, modlen);
    return 0;
}

/* PKCS#1 v1.5: 00 01 FF..FF 00 DigestInfo hash */
static inline int rsa_verify_pkcs1_sha256(const uint8_t *mod, uint32_t modlen,
                                          uint32_t exp, const uint8_t *sig,
                                          uint32_t siglen,
                                          const uint8_t *hash)
{
    static uint8_t em[RSA_MAX_BYTES];

    if (rsa_public(mod, modlen, exp, sig, siglen, em) < 0)
        return 0;

    uint32_t prefix_len = sizeof(rsa_sha256_prefix);
    uint32_t need = 3 + 8 + prefix_len + 32;      /* minimalne dopelnienie */
    if (modlen < need)
        return 0;

    if (em[0] != 0x00 || em[1] != 0x01)
        return 0;

    uint32_t i = 2;
    while (i < modlen && em[i] == 0xff)
        i++;
    if (i < 10 || i >= modlen || em[i] != 0x00)   /* min. 8 bajtow 0xFF */
        return 0;
    i++;

    if (modlen - i != prefix_len + 32)
        return 0;
    for (uint32_t k = 0; k < prefix_len; k++)
        if (em[i + k] != rsa_sha256_prefix[k])
            return 0;

    uint8_t diff = 0;
    for (int k = 0; k < 32; k++)
        diff |= (uint8_t)(em[i + prefix_len + k] ^ hash[k]);
    return diff == 0;
}

/* MGF1 z SHA-256 (RFC 8017 B.2.1) */
static inline void rsa_mgf1(const uint8_t *seed, uint32_t seedlen,
                            uint8_t *mask, uint32_t masklen)
{
    uint8_t buf[64 + 4];
    uint8_t hash[32];
    uint32_t done = 0, counter = 0;

    while (done < masklen) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < seedlen && n < sizeof(buf) - 4; i++)
            buf[n++] = seed[i];
        buf[n++] = (uint8_t)(counter >> 24);
        buf[n++] = (uint8_t)(counter >> 16);
        buf[n++] = (uint8_t)(counter >> 8);
        buf[n++] = (uint8_t)counter;
        counter++;

        sha256(buf, n, hash);
        uint32_t take = masklen - done;
        if (take > 32)
            take = 32;
        for (uint32_t i = 0; i < take; i++)
            mask[done + i] = hash[i];
        done += take;
    }
}

/* EMSA-PSS-VERIFY z SHA-256 i sol dlugosci skrotu (tak robi TLS 1.3) */
static inline int rsa_verify_pss_sha256(const uint8_t *mod, uint32_t modlen,
                                        uint32_t exp, const uint8_t *sig,
                                        uint32_t siglen, const uint8_t *hash,
                                        uint32_t modbits)
{
    static uint8_t em[RSA_MAX_BYTES];
    static uint8_t db[RSA_MAX_BYTES];
    static uint8_t mask[RSA_MAX_BYTES];
    uint8_t mprime[8 + 32 + 32];
    uint8_t hprime[32];

    const uint32_t hlen = 32, slen = 32;
    if (modlen < hlen + slen + 2)
        return 0;
    if (rsa_public(mod, modlen, exp, sig, siglen, em) < 0)
        return 0;
    if (em[modlen - 1] != 0xbc)
        return 0;

    uint32_t dblen = modlen - hlen - 1;
    const uint8_t *h = em + dblen;

    rsa_mgf1(h, hlen, mask, dblen);
    for (uint32_t i = 0; i < dblen; i++)
        db[i] = (uint8_t)(em[i] ^ mask[i]);

    /* wyzeruj bity ponad emBits = modbits - 1 */
    uint32_t unused = 8 * modlen - (modbits - 1);
    if (unused > 0 && unused < 8)
        db[0] &= (uint8_t)(0xff >> unused);
    else if (unused >= 8)
        return 0;

    uint32_t i = 0;
    while (i < dblen && db[i] == 0)
        i++;
    if (i >= dblen || db[i] != 0x01)
        return 0;
    i++;
    if (dblen - i != slen)
        return 0;

    uint32_t n = 0;
    for (int k = 0; k < 8; k++)
        mprime[n++] = 0;
    for (uint32_t k = 0; k < hlen; k++)
        mprime[n++] = hash[k];
    for (uint32_t k = 0; k < slen; k++)
        mprime[n++] = db[i + k];

    sha256(mprime, n, hprime);

    uint8_t diff = 0;
    for (uint32_t k = 0; k < hlen; k++)
        diff |= (uint8_t)(hprime[k] ^ h[k]);
    return diff == 0;
}

#endif
