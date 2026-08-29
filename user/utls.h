/* Klient TLS 1.3 (RFC 8446) - minimalny, ale prawdziwy
 *
 * Zestaw: X25519 do wymiany kluczy, TLS_CHACHA20_POLY1305_SHA256 do
 * szyfrowania, SHA-256 w transkrypcie i harmonogramie kluczy. Tyle
 * wystarcza, zeby dogadac sie z dowolnym wspolczesnym serwerem -
 * TLS 1.3 wyrzucil z protokolu cala starzyzne.
 *
 * Przebieg: ClientHello z kluczem publicznym -> ServerHello z jego
 * kluczem -> obie strony licza ten sam sekret -> reszta uzgodnien
 * leci juz zaszyfrowana -> Finished po obu stronach -> dane aplikacji
 * na osobnych kluczach.
 *
 * Certyfikat serwera jest sprawdzany, gdy podamy magazyn zaufania:
 * podpis w CertificateVerify (kluczem z certyfikatu), lancuch podpisow
 * do zaufanego korzenia, daty waznosci i nazwa hosta. Bez magazynu
 * polaczenie jest tylko szyfrowane - i wtedy jest to jasno powiedziane
 * wolajacemu (pole 'verified').
 *
 * Czego jeszcze nie ma: podpisow ECDSA (czesc serwerow ich uzywa),
 * odwolan certyfikatow i sprawdzania ograniczen nazw.
 */

#ifndef PROTONOS_UTLS_H
#define PROTONOS_UTLS_H

#include "uapi.h"
#include "usha256.h"
#include "uchacha.h"
#include "ux25519.h"
#include "urand.h"
#include "ucert.h"

#define TLS_PLAINTEXT_MAX 16384
#define TLS_RECORD_MAX    (TLS_PLAINTEXT_MAX + 256)
#define TLS_HS_MAX        20480          /* lancuch certyfikatow bywa duzy */

/* Typy rekordow */
#define REC_CCS       20
#define REC_ALERT     21
#define REC_HANDSHAKE 22
#define REC_APPDATA   23

/* Typy wiadomosci uzgodnien */
#define HS_CLIENT_HELLO   1
#define HS_SERVER_HELLO   2
#define HS_NEW_TICKET     4
#define HS_ENC_EXTENSIONS 8
#define HS_CERTIFICATE    11
#define HS_CERT_VERIFY    15
#define HS_FINISHED       20

#define TLS_AES_GCM_SHA256   0x1301
#define TLS_CHACHA20_SHA256  0x1303
#define GROUP_X25519         0x001d

/* Co i wzgledem czego sprawdzac */
struct tls_config {
    int verify;                          /* 1 = wymagaj poprawnego lancucha */
    const uint8_t *roots;                /* certyfikaty DER, jeden za drugim */
    uint32_t roots_len;
};

struct tls_conn {
    int32_t sock;
    int established;
    int verified;                        /* 1 = certyfikat sprawdzony */
    int verify_err;                      /* CERT_* gdy sie nie udalo */

    uint8_t key_c[32], iv_c[12];         /* klucze aplikacyjne */
    uint8_t key_s[32], iv_s[12];
    uint32_t seq_c, seq_s;               /* liczniki rekordow */

    uint8_t app[TLS_RECORD_MAX];         /* odszyfrowane dane aplikacji */
    uint32_t applen, appoff;

    const char *error;
};

/* --- pomocnicze: zapis liczb --- */

static inline void tls_put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline uint32_t tls_get16(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | p[1];
}

static inline uint32_t tls_get24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/* --- warstwa gniazda --- */

static inline int tls_write_all(struct tls_conn *c, const uint8_t *data,
                                uint32_t len)
{
    uint32_t done = 0;
    while (done < len) {
        uint32_t take = len - done;
        if (take > 1024)                 /* jadro wysyla po kawalku i czeka */
            take = 1024;
        if (tcp_send(c->sock, data + done, take, 10000) < 0)
            return -1;
        done += take;
    }
    return 0;
}

static inline int tls_read_all(struct tls_conn *c, uint8_t *buf, uint32_t len)
{
    uint32_t done = 0;
    while (done < len) {
        int32_t n = tcp_recv(c->sock, buf + done, len - done, 10000);
        if (n <= 0)
            return -1;
        done += (uint32_t)n;
    }
    return 0;
}

/* Odbiera jeden rekord: naglowek 5 B + tresc */
static inline int tls_read_record(struct tls_conn *c, uint8_t *type,
                                  uint8_t *hdr, uint8_t *buf, uint32_t *len)
{
    if (tls_read_all(c, hdr, 5) < 0)
        return -1;
    uint32_t n = tls_get16(hdr + 3);
    if (n > TLS_RECORD_MAX)
        return -1;
    if (tls_read_all(c, buf, n) < 0)
        return -1;
    *type = hdr[0];
    *len = n;
    return 0;
}

/* --- szyfrowanie rekordow --- */

/* nonce = iv XOR licznik rekordu (na osmiu ostatnich bajtach) */
static inline void tls_nonce(const uint8_t *iv, uint32_t seq, uint8_t *out)
{
    for (int i = 0; i < 12; i++)
        out[i] = iv[i];
    for (int i = 0; i < 4; i++)
        out[11 - i] ^= (uint8_t)(seq >> (i * 8));
}

/* Wysyla zaszyfrowany rekord: tresc || wlasciwy typ */
static inline int tls_send_encrypted(struct tls_conn *c, uint8_t inner_type,
                                     const uint8_t *data, uint32_t len)
{
    static uint8_t plain[TLS_RECORD_MAX];
    static uint8_t out[TLS_RECORD_MAX + 5];
    uint8_t nonce[12], tag[16];

    if (len + 1 > TLS_RECORD_MAX)
        return -1;

    for (uint32_t i = 0; i < len; i++)
        plain[i] = data[i];
    plain[len] = inner_type;
    uint32_t plen = len + 1;

    out[0] = REC_APPDATA;                /* na zewnatrz zawsze "dane" */
    out[1] = 0x03;
    out[2] = 0x03;
    tls_put16(out + 3, plen + 16);

    tls_nonce(c->iv_c, c->seq_c, nonce);
    aead_encrypt(c->key_c, nonce, out, 5, plain, out + 5, plen, tag);
    for (int i = 0; i < 16; i++)
        out[5 + plen + i] = tag[i];
    c->seq_c++;

    return tls_write_all(c, out, 5 + plen + 16);
}

/* Odbiera i odszyfrowuje rekord. Zwraca dlugosc tresci albo -1;
   wlasciwy typ wiadomosci ladnie w *inner_type. */
static inline int tls_recv_encrypted(struct tls_conn *c, uint8_t *inner_type,
                                     uint8_t *out, uint32_t max)
{
    static uint8_t buf[TLS_RECORD_MAX];
    uint8_t hdr[5], nonce[12];
    uint8_t type;
    uint32_t len;

    for (;;) {
        if (tls_read_record(c, &type, hdr, buf, &len) < 0)
            return -1;
        if (type == REC_CCS)
            continue;                    /* zgodnosc ze starym sprzetem */
        if (type == REC_ALERT && len >= 2) {
            c->error = "serwer zerwal polaczenie (alert)";
            return -1;
        }
        if (len < 17 || len - 16 > max)
            return -1;

        uint32_t ctlen = len - 16;
        tls_nonce(c->iv_s, c->seq_s, nonce);
        if (!aead_decrypt(c->key_s, nonce, hdr, 5, buf, out, ctlen,
                          buf + ctlen)) {
            c->error = "nie udalo sie odszyfrowac rekordu";
            return -1;
        }
        c->seq_s++;

        while (ctlen > 0 && out[ctlen - 1] == 0)   /* dopelnienie zerami */
            ctlen--;
        if (!ctlen)
            continue;
        *inner_type = out[ctlen - 1];
        return (int)(ctlen - 1);
    }
}

/* --- harmonogram kluczy (RFC 8446 7.1) --- */

static inline void tls_traffic_keys(const uint8_t *secret, uint8_t *key,
                                    uint8_t *iv)
{
    hkdf_expand_label(secret, "key", 0, 0, key, 32);
    hkdf_expand_label(secret, "iv", 0, 0, iv, 12);
}

/* --- sprawdzenie certyfikatow --- */

/* Rozbiera wiadomosc Certificate, sprawdza podpis z CertificateVerify
   (to on dowodzi, ze serwer ma klucz prywatny do certyfikatu) i caly
   lancuch. Zwraca CERT_OK albo kod bledu. */
static inline int tls_check_certs(const uint8_t *certmsg, uint32_t certlen,
                                  const uint8_t *cv, uint32_t cvlen,
                                  const uint8_t *cv_hash, const char *host,
                                  const uint8_t *roots, uint32_t roots_len)
{
    struct x509_cert chain[CERT_CHAIN_MAX];
    int n = 0;

    if (certlen < 8)
        return CERT_ERR_PARSE;

    const uint8_t *p = certmsg + 4;      /* za naglowkiem wiadomosci */
    const uint8_t *end = certmsg + certlen;
    p += 1 + p[0];                       /* certificate_request_context */
    if (p + 3 > end)
        return CERT_ERR_PARSE;
    uint32_t list_len = tls_get24(p);
    p += 3;
    const uint8_t *list_end = p + list_len;
    if (list_end > end)
        list_end = end;

    while (p + 3 <= list_end && n < CERT_CHAIN_MAX) {
        uint32_t clen = tls_get24(p);
        p += 3;
        if (p + clen > list_end)
            break;
        if (x509_parse(&chain[n], p, clen) == 0)
            n++;
        p += clen;
        if (p + 2 > list_end)
            break;
        uint32_t extlen = tls_get16(p);  /* rozszerzenia wpisu */
        p += 2 + extlen;
    }
    if (!n)
        return CERT_ERR_PARSE;

    /* CertificateVerify: algorytm, dlugosc, podpis */
    if (cvlen < 8)
        return CERT_ERR_PARSE;
    uint32_t alg = tls_get16(cv + 4);
    uint32_t siglen = tls_get16(cv + 6);
    const uint8_t *sig = cv + 8;
    if (8 + siglen > cvlen)
        return CERT_ERR_PARSE;

    /* Podpisywana tresc wedlug RFC 8446 4.4.3 */
    static uint8_t tosign[64 + 34 + 32];
    uint32_t m = 0;
    for (int i = 0; i < 64; i++)
        tosign[m++] = 0x20;
    const char *label = "TLS 1.3, server CertificateVerify";
    for (int i = 0; label[i]; i++)
        tosign[m++] = (uint8_t)label[i];
    tosign[m++] = 0x00;
    for (int i = 0; i < 32; i++)
        tosign[m++] = cv_hash[i];

    uint8_t hash[32];
    sha256(tosign, m, hash);

    const struct x509_cert *leaf = &chain[0];
    int ok = 0;

    if (leaf->key_type == X509_KEY_EC) {
        struct ec_curve *cv;
        uint8_t echash[64];
        uint32_t hashlen;

        /* 0x0403 = ecdsa_secp256r1_sha256, 0x0503 = ..._secp384r1_sha384 */
        if (alg == 0x0403 && leaf->ec_bits == 256) {
            cv = &ec_p256;
            for (int k = 0; k < 32; k++)
                echash[k] = hash[k];
            hashlen = 32;
        } else if (alg == 0x0503 && leaf->ec_bits == 384) {
            cv = &ec_p384;
            sha384(tosign, m, echash);   /* P-384 podpisuje sie SHA-384 */
            hashlen = 48;
        } else {
            return CERT_ERR_ALG;
        }
        if (!leaf->ec_point)
            return CERT_ERR_ALG;

        if (!ecdsa_verify_der(cv, leaf->ec_point, leaf->ec_point_len, echash,
                              hashlen, sig, siglen))
            return CERT_ERR_SIGNATURE;
        return cert_verify_chain(chain, n, roots, roots_len, host);
    }

    if (leaf->key_type != X509_KEY_RSA)
        return CERT_ERR_ALG;
    if (alg == 0x0804)                   /* rsa_pss_rsae_sha256 */
        ok = rsa_verify_pss_sha256(leaf->mod, leaf->mod_len, leaf->exp,
                                   sig, siglen, hash, leaf->mod_len * 8);
    else if (alg == 0x0401)              /* rsa_pkcs1_sha256 */
        ok = rsa_verify_pkcs1_sha256(leaf->mod, leaf->mod_len, leaf->exp,
                                     sig, siglen, hash);
    else
        return CERT_ERR_ALG;

    if (!ok)
        return CERT_ERR_SIGNATURE;

    return cert_verify_chain(chain, n, roots, roots_len, host);
}

/* --- uzgodnienie --- */

/* Zamyka polaczenie po nieudanym uzgodnieniu - inaczej gniazdo TCP
   zostaje otwarte az do konca zycia procesu (a slotow jest kilka) */
static inline void tls_abort(struct tls_conn *c)
{
    if (c->sock >= 0)
        tcp_close(c->sock);
    c->sock = -1;
    c->established = 0;
}

static inline int tls_connect(struct tls_conn *c, uint32_t ip, uint32_t port,
                              const char *sni, const struct tls_config *cfg)
{
    static uint8_t msg[TLS_HS_MAX];
    static uint8_t hs[TLS_HS_MAX];
    uint8_t priv[32], pub[32], shared[32];
    uint8_t early[32], derived[32], hs_secret[32], master[32];
    uint8_t c_hs[32], s_hs[32], c_ap[32], s_ap[32];
    uint8_t hs_key_c[32], hs_iv_c[12], hs_key_s[32], hs_iv_s[12];
    uint8_t empty_hash[32], transcript[32];
    struct sha256 tr;

    c->error = 0;
    c->established = 0;
    c->verified = 0;
    c->verify_err = CERT_ERR_PARSE;
    c->applen = c->appoff = 0;
    c->seq_c = c->seq_s = 0;

    c->sock = tcp_connect(ip, port, 8000);
    if (c->sock < 0) {
        c->error = "nie udalo sie polaczyc";
        return -1;
    }

    urand_bytes(priv, 32);
    x25519_base(pub, priv);
    sha256_init(&tr);

    /* --- ClientHello --- */
    uint32_t n = 0;
    msg[n++] = HS_CLIENT_HELLO;
    n += 3;                              /* miejsce na dlugosc */
    uint32_t body = n;

    msg[n++] = 0x03; msg[n++] = 0x03;    /* legacy_version */
    urand_bytes(msg + n, 32);            /* random */
    n += 32;
    msg[n++] = 32;                       /* legacy_session_id */
    urand_bytes(msg + n, 32);
    n += 32;
    tls_put16(msg + n, 2); n += 2;       /* jeden zestaw szyfrow */
    tls_put16(msg + n, TLS_CHACHA20_SHA256); n += 2;
    msg[n++] = 1; msg[n++] = 0;          /* brak kompresji */

    uint32_t ext_len_at = n;
    n += 2;
    uint32_t ext_start = n;

    /* server_name (SNI) */
    uint32_t sni_len = 0;
    while (sni[sni_len])
        sni_len++;
    tls_put16(msg + n, 0x0000); n += 2;
    tls_put16(msg + n, sni_len + 5); n += 2;
    tls_put16(msg + n, sni_len + 3); n += 2;
    msg[n++] = 0;                        /* host_name */
    tls_put16(msg + n, sni_len); n += 2;
    for (uint32_t i = 0; i < sni_len; i++)
        msg[n++] = (uint8_t)sni[i];

    /* supported_versions: tylko TLS 1.3 */
    tls_put16(msg + n, 0x002b); n += 2;
    tls_put16(msg + n, 3); n += 2;
    msg[n++] = 2;
    tls_put16(msg + n, 0x0304); n += 2;

    /* supported_groups: x25519 */
    tls_put16(msg + n, 0x000a); n += 2;
    tls_put16(msg + n, 4); n += 2;
    tls_put16(msg + n, 2); n += 2;
    tls_put16(msg + n, GROUP_X25519); n += 2;

    /* signature_algorithms - serwer bez tego odmawia */
    tls_put16(msg + n, 0x000d); n += 2;
    tls_put16(msg + n, 10); n += 2;      /* dlugosc rozszerzenia */
    tls_put16(msg + n, 8); n += 2;       /* dlugosc listy algorytmow */
    tls_put16(msg + n, 0x0804); n += 2;  /* rsa_pss_rsae_sha256 */
    tls_put16(msg + n, 0x0403); n += 2;  /* ecdsa_secp256r1_sha256 */
    tls_put16(msg + n, 0x0503); n += 2;  /* ecdsa_secp384r1_sha384 */
    tls_put16(msg + n, 0x0401); n += 2;  /* rsa_pkcs1_sha256 */

    /* key_share z naszym kluczem publicznym */
    tls_put16(msg + n, 0x0033); n += 2;
    tls_put16(msg + n, 38); n += 2;
    tls_put16(msg + n, 36); n += 2;
    tls_put16(msg + n, GROUP_X25519); n += 2;
    tls_put16(msg + n, 32); n += 2;
    for (int i = 0; i < 32; i++)
        msg[n++] = pub[i];

    tls_put16(msg + ext_len_at, n - ext_start);
    uint32_t blen = n - body;
    msg[1] = (uint8_t)(blen >> 16);
    msg[2] = (uint8_t)(blen >> 8);
    msg[3] = (uint8_t)blen;

    sha256_update(&tr, msg, n);

    uint8_t rec[5];
    rec[0] = REC_HANDSHAKE; rec[1] = 0x03; rec[2] = 0x01;
    tls_put16(rec + 3, n);
    if (tls_write_all(c, rec, 5) < 0 || tls_write_all(c, msg, n) < 0) {
        c->error = "nie udalo sie wyslac ClientHello";
        tls_abort(c);
        return -1;
    }

    /* --- ServerHello --- */
    uint8_t type;
    uint8_t hdr[5];
    uint32_t len;
    if (tls_read_record(c, &type, hdr, msg, &len) < 0 ||
        type != REC_HANDSHAKE || len < 4 || msg[0] != HS_SERVER_HELLO) {
        c->error = "brak ServerHello";
        tls_abort(c);
        return -1;
    }
    sha256_update(&tr, msg, len);

    uint32_t p = 4;
    p += 2;                              /* legacy_version */
    p += 32;                             /* random */
    if (p >= len) {
        c->error = "uszkodzony ServerHello";
        tls_abort(c);
        return -1;
    }
    p += 1 + msg[p];                     /* legacy_session_id */
    uint32_t suite = tls_get16(msg + p);
    p += 2;
    p += 1;                              /* kompresja */
    if (suite != TLS_CHACHA20_SHA256) {
        c->error = "serwer wybral inny zestaw szyfrow";
        tls_abort(c);
        return -1;
    }

    uint32_t exts = tls_get16(msg + p);
    p += 2;
    uint32_t ext_end = p + exts;
    uint8_t server_pub[32];
    int have_key = 0;

    while (p + 4 <= ext_end && p + 4 <= len) {
        uint32_t etype = tls_get16(msg + p);
        uint32_t elen = tls_get16(msg + p + 2);
        uint32_t edata = p + 4;
        if (etype == 0x0033 && elen >= 36) {          /* key_share */
            if (tls_get16(msg + edata) == GROUP_X25519 &&
                tls_get16(msg + edata + 2) == 32) {
                for (int i = 0; i < 32; i++)
                    server_pub[i] = msg[edata + 4 + i];
                have_key = 1;
            }
        }
        p = edata + elen;
    }
    if (!have_key) {
        c->error = "ServerHello bez klucza x25519";
        tls_abort(c);
        return -1;
    }

    /* --- wspolny sekret i klucze uzgodnien --- */
    x25519(shared, priv, server_pub);

    uint8_t zeros[32];
    for (int i = 0; i < 32; i++)
        zeros[i] = 0;

    sha256("", 0, empty_hash);
    hkdf_extract(0, 0, zeros, 32, early);
    hkdf_expand_label(early, "derived", empty_hash, 32, derived, 32);
    hkdf_extract(derived, 32, shared, 32, hs_secret);

    struct sha256 snap = tr;             /* transkrypt: ClientHello..ServerHello */
    sha256_final(&snap, transcript);
    hkdf_expand_label(hs_secret, "c hs traffic", transcript, 32, c_hs, 32);
    hkdf_expand_label(hs_secret, "s hs traffic", transcript, 32, s_hs, 32);
    tls_traffic_keys(c_hs, hs_key_c, hs_iv_c);
    tls_traffic_keys(s_hs, hs_key_s, hs_iv_s);

    /* Od tej chwili rekordy sa szyfrowane kluczami uzgodnien */
    for (int i = 0; i < 32; i++) {
        c->key_s[i] = hs_key_s[i];
        c->key_c[i] = hs_key_c[i];
    }
    for (int i = 0; i < 12; i++) {
        c->iv_s[i] = hs_iv_s[i];
        c->iv_c[i] = hs_iv_c[i];
    }
    c->seq_c = c->seq_s = 0;

    /* --- reszta uzgodnien (juz zaszyfrowana) --- */
    static uint8_t cert_msg[TLS_HS_MAX];
    static uint8_t cv_msg[1024];
    uint32_t cert_msg_len = 0, cv_msg_len = 0;
    uint8_t cv_hash[32];

    uint32_t hslen = 0;
    int finished_seen = 0;
    uint8_t before_finished[32];

    while (!finished_seen) {
        uint8_t inner;
        int got = tls_recv_encrypted(c, &inner, msg, TLS_HS_MAX);
        if (got < 0) {
            if (!c->error)
                c->error = "przerwane uzgodnienie";
            tls_abort(c);
            tls_abort(c);
        return -1;
        }
        if (inner != REC_HANDSHAKE)
            continue;

        for (int i = 0; i < got && hslen < TLS_HS_MAX; i++)
            hs[hslen++] = msg[i];

        /* w jednym rekordzie moze byc kilka wiadomosci - i odwrotnie */
        uint32_t off = 0;
        while (off + 4 <= hslen) {
            uint32_t mlen = tls_get24(hs + off + 1);
            if (off + 4 + mlen > hslen)
                break;                   /* wiadomosc jeszcze nie w calosci */

            uint8_t mtype = hs[off];

            if (mtype == HS_CERTIFICATE && 4 + mlen <= TLS_HS_MAX) {
                cert_msg_len = 4 + mlen;
                for (uint32_t i = 0; i < cert_msg_len; i++)
                    cert_msg[i] = hs[off + i];
            } else if (mtype == HS_CERT_VERIFY && 4 + mlen <= sizeof(cv_msg)) {
                /* podpis pokrywa transkrypt do konca wiadomosci
                   Certificate - czyli stan sprzed tej wiadomosci */
                struct sha256 sc = tr;
                sha256_final(&sc, cv_hash);
                cv_msg_len = 4 + mlen;
                for (uint32_t i = 0; i < cv_msg_len; i++)
                    cv_msg[i] = hs[off + i];
            }

            if (mtype == HS_FINISHED) {
                struct sha256 s2 = tr;   /* transkrypt bez Finished serwera */
                sha256_final(&s2, before_finished);

                uint8_t fkey[32], expect[32];
                hkdf_expand_label(s_hs, "finished", 0, 0, fkey, 32);
                hmac_sha256(fkey, 32, before_finished, 32, expect);

                int bad = (mlen != 32);
                for (uint32_t i = 0; !bad && i < 32; i++)
                    if (expect[i] != hs[off + 4 + i])
                        bad = 1;
                if (bad) {
                    c->error = "Finished serwera sie nie zgadza";
                    tls_abort(c);
                    tls_abort(c);
            tls_abort(c);
        return -1;
                }
                finished_seen = 1;
            }

            sha256_update(&tr, hs + off, 4 + mlen);
            off += 4 + mlen;
            if (finished_seen)
                break;
        }

        /* przesun to, co zostalo niedokonczone */
        uint32_t rest = hslen - off;
        for (uint32_t i = 0; i < rest; i++)
            hs[i] = hs[off + i];
        hslen = rest;
    }

    /* --- czy to na pewno ten serwer? --- */
    if (cert_msg_len && cv_msg_len) {
        c->verify_err = tls_check_certs(cert_msg, cert_msg_len, cv_msg,
                                        cv_msg_len, cv_hash, sni,
                                        cfg ? cfg->roots : 0,
                                        cfg ? cfg->roots_len : 0);
        c->verified = (c->verify_err == CERT_OK);
    } else {
        c->verify_err = CERT_ERR_PARSE;
    }

    if (cfg && cfg->verify && !c->verified) {
        c->error = cert_error_text(c->verify_err);
        tls_abort(c);
        return -1;
    }

    /* --- nasze Finished --- */
    struct sha256 s3 = tr;               /* transkrypt z Finished serwera */
    sha256_final(&s3, transcript);

    uint8_t fkey_c[32], verify[32];
    hkdf_expand_label(c_hs, "finished", 0, 0, fkey_c, 32);
    hmac_sha256(fkey_c, 32, transcript, 32, verify);

    uint8_t fin[36];
    fin[0] = HS_FINISHED;
    fin[1] = 0; fin[2] = 0; fin[3] = 32;
    for (int i = 0; i < 32; i++)
        fin[4 + i] = verify[i];
    if (tls_send_encrypted(c, REC_HANDSHAKE, fin, 36) < 0) {
        c->error = "nie udalo sie wyslac Finished";
        tls_abort(c);
        return -1;
    }

    /* --- klucze aplikacyjne --- */
    hkdf_expand_label(hs_secret, "derived", empty_hash, 32, derived, 32);
    hkdf_extract(derived, 32, zeros, 32, master);
    hkdf_expand_label(master, "c ap traffic", transcript, 32, c_ap, 32);
    hkdf_expand_label(master, "s ap traffic", transcript, 32, s_ap, 32);
    tls_traffic_keys(c_ap, c->key_c, c->iv_c);
    tls_traffic_keys(s_ap, c->key_s, c->iv_s);
    c->seq_c = c->seq_s = 0;

    c->established = 1;
    return 0;
}

/* --- dane aplikacji --- */

static inline int tls_send(struct tls_conn *c, const void *data, uint32_t len)
{
    if (!c->established)
        return -1;
    return tls_send_encrypted(c, REC_APPDATA, data, len);
}

/* Zwraca >0 = bajty, 0 = koniec strumienia, -1 = blad */
static inline int tls_recv(struct tls_conn *c, void *buf, uint32_t max)
{
    uint8_t *out = buf;

    if (c->appoff < c->applen) {         /* zostalo z poprzedniego rekordu */
        uint32_t take = c->applen - c->appoff;
        if (take > max)
            take = max;
        for (uint32_t i = 0; i < take; i++)
            out[i] = c->app[c->appoff + i];
        c->appoff += take;
        return (int)take;
    }

    for (;;) {
        uint8_t inner;
        int got = tls_recv_encrypted(c, &inner, c->app, TLS_RECORD_MAX);
        if (got < 0)
            return c->error ? -1 : 0;
        if (inner == REC_HANDSHAKE)
            continue;                    /* np. bilet sesyjny - pomijamy */
        if (inner == REC_ALERT)
            return 0;                    /* close_notify albo blad */
        if (inner != REC_APPDATA)
            continue;

        c->applen = (uint32_t)got;
        c->appoff = 0;
        uint32_t take = c->applen;
        if (take > max)
            take = max;
        for (uint32_t i = 0; i < take; i++)
            out[i] = c->app[i];
        c->appoff = take;
        return (int)take;
    }
}

static inline void tls_close(struct tls_conn *c)
{
    if (c->sock >= 0)
        tcp_close(c->sock);
    c->sock = -1;
    c->established = 0;
}

#endif
