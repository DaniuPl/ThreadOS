/* certtest - sprawdzanie certyfikatow X.509 i podpisow RSA
 *
 * Bierze prawdziwy certyfikat (wygenerowany openssl-em, wbudowany w
 * certdata.h), rozbiera go parserem DER i sprawdza jego wlasny podpis.
 * Certyfikat jest samopodpisany, wiec do weryfikacji wystarczy klucz,
 * ktory sam w sobie niesie - nie trzeba zadnego magazynu zaufania.
 *
 * Dodatkowo: podpis w schemacie PSS (tego wymaga TLS 1.3), to samo dla
 * certyfikatu na krzywej P-256 (ECDSA) oraz proba podmiany bajtu -
 * podpis musi wtedy odpasc.
 */

#include "uapi.h"
#include "usha256.h"
#include "ursa.h"
#include "ux509.h"
#include "uecdsa.h"
#include "usha512.h"
#include "certdata.h"
#include "ucert.h"

static int failures;

static void result(const char *what, int ok)
{
    print(what);
    print(ok ? ": OK\n" : ": BLAD\n");
    if (!ok)
        failures++;
}

static void print_str(const char *label, const char *s)
{
    print(label);
    print(s);
    print("\n");
}

void _start(void)
{
    struct x509_cert cert;
    uint8_t hash[32];

    print("[cert] parser X.509 i podpisy RSA\n");

    result("rozbior certyfikatu DER",
           x509_parse(&cert, cert_der, sizeof(cert_der)) == 0);
    if (failures)
        sys_exit(1);

    print("      wazny od ");
    print(cert.not_before);
    print(" do ");
    print(cert.not_after);
    print("\n");

    result("klucz publiczny RSA znaleziony",
           cert.key_type == X509_KEY_RSA && cert.mod_len == 256);
    result("wykladnik publiczny to 65537", cert.exp == 65537);
    result("algorytm podpisu: RSA + SHA-256",
           cert.sig_alg == X509_ALG_RSA_SHA256);
    result("dlugosc podpisu zgadza sie z kluczem",
           cert.sig_len == cert.mod_len);

    /* Podpis pokrywa bajty tbsCertificate */
    sha256(cert.tbs, cert.tbs_len, hash);
    result("podpis wlasny certyfikatu (PKCS#1 v1.5)",
           rsa_verify_pkcs1_sha256(cert.mod, cert.mod_len, cert.exp,
                                   cert.sig, cert.sig_len, hash));

    /* Ta sama weryfikacja po podmianie bajtu tresci musi odpasc */
    static uint8_t tampered[4096];
    for (uint32_t i = 0; i < sizeof(cert_der); i++)
        tampered[i] = cert_der[i];
    tampered[cert.tbs_len / 2] ^= 0x01;

    struct x509_cert bad;
    if (x509_parse(&bad, tampered, sizeof(cert_der)) == 0) {
        sha256(bad.tbs, bad.tbs_len, hash);
        result("podmieniona tresc odrzucona",
               !rsa_verify_pkcs1_sha256(bad.mod, bad.mod_len, bad.exp,
                                        bad.sig, bad.sig_len, hash));
    } else {
        result("podmieniona tresc odrzucona (juz na parserze)", 1);
    }

    /* Podpis PSS - tego wymaga TLS 1.3 w CertificateVerify */
    uint32_t msglen = 0;
    while (pss_msg[msglen])
        msglen++;
    sha256(pss_msg, msglen, hash);
    result("podpis w schemacie PSS",
           rsa_verify_pss_sha256(cert.mod, cert.mod_len, cert.exp,
                                 pss_sig, sizeof(pss_sig), hash, 2048));

    hash[0] ^= 0x01;
    result("PSS: zly skrot odrzucony",
           !rsa_verify_pss_sha256(cert.mod, cert.mod_len, cert.exp,
                                  pss_sig, sizeof(pss_sig), hash, 2048));

    /* Dopasowanie nazwy hosta z SubjectAltName albo jej brak */
    if (cert.san) {
        result("SubjectAltName: pasuje 'protonos.test'",
               x509_matches_host(&cert, "protonos.test"));
        result("SubjectAltName: obca nazwa odrzucona",
               !x509_matches_host(&cert, "zly.przyklad"));
    } else {
        print_str("      ", "certyfikat bez SubjectAltName (stary styl)");
    }

    /* --- certyfikat na krzywej P-256 --- */
    struct x509_cert ec;
    result("rozbior certyfikatu ECDSA",
           x509_parse(&ec, ec_cert_der, sizeof(ec_cert_der)) == 0);
    result("klucz na krzywej P-256 rozpoznany",
           ec.key_type == X509_KEY_EC && ec.ec_bits == 256 &&
           ec.ec_point_len == 65);
    result("algorytm podpisu: ECDSA + SHA-256",
           ec.sig_alg == X509_ALG_ECDSA_SHA256);

    uint32_t t0, t1;
    t0 = sys_getticks();
    sha256(ec.tbs, ec.tbs_len, hash);
    result("podpis wlasny certyfikatu (ECDSA P-256)",
           ecdsa_verify_der(&ec_p256, ec.ec_point, ec.ec_point_len, hash, 32,
                            ec.sig, ec.sig_len));
    t1 = sys_getticks();
    print("      sprawdzenie podpisu ECDSA zajelo ~");
    print_dec((t1 - t0) * 10);
    print(" ms\n");

    hash[0] ^= 0x01;
    result("ECDSA: zly skrot odrzucony",
           !ecdsa_verify_der(&ec_p256, ec.ec_point, ec.ec_point_len, hash, 32,
                             ec.sig, ec.sig_len));

    sha256(pss_msg, msglen, hash);
    result("podpis ECDSA wiadomosci (osobny wektor)",
           ecdsa_verify_der(&ec_p256, ec.ec_point, ec.ec_point_len, hash, 32,
                            ec_sig, sizeof(ec_sig)));

    /* --- certyfikat na krzywej P-384 (SHA-384) --- */
    struct x509_cert ec384;
    uint8_t hash384[48];

    result("rozbior certyfikatu P-384",
           x509_parse(&ec384, p384_cert_der, sizeof(p384_cert_der)) == 0);
    result("klucz na krzywej P-384 rozpoznany",
           ec384.key_type == X509_KEY_EC && ec384.ec_bits == 384 &&
           ec384.ec_point_len == 97);
    result("algorytm podpisu: ECDSA + SHA-384",
           ec384.sig_alg == X509_ALG_ECDSA_SHA384);

    t0 = sys_getticks();
    sha384(ec384.tbs, ec384.tbs_len, hash384);
    result("podpis wlasny certyfikatu (ECDSA P-384)",
           ecdsa_verify_der(&ec_p384, ec384.ec_point, ec384.ec_point_len,
                            hash384, 48, ec384.sig, ec384.sig_len));
    t1 = sys_getticks();
    print("      sprawdzenie podpisu P-384 zajelo ~");
    print_dec((t1 - t0) * 10);
    print(" ms\n");

    hash384[0] ^= 0x01;
    result("P-384: zly skrot odrzucony",
           !ecdsa_verify_der(&ec_p384, ec384.ec_point, ec384.ec_point_len,
                             hash384, 48, ec384.sig, ec384.sig_len));

    /* SHA-384 na znanym wektorze (FIPS 180-4) */
    static const uint8_t want384[] = {
        0xcb,0x00,0x75,0x3f,0x45,0xa3,0x5e,0x8b,0xb5,0xa0,0x3d,0x69,0x9a,0xc6,
        0x50,0x07,0x27,0x2c,0x32,0xab,0x0e,0xde,0xd1,0x63,0x1a,0x8b,0x60,0x5a,
        0x43,0xff,0x5b,0xed,0x80,0x86,0x07,0x2b,0xa1,0xe7,0xcc,0x23,0x58,0xba,
        0xec,0xa1,0x34,0xc8,0x25,0xa7 };
    sha384("abc", 3, hash384);
    int same = 1;
    for (int i = 0; i < 48; i++)
        if (hash384[i] != want384[i])
            same = 0;
    result("SHA-384('abc') zgadza sie z FIPS 180-4", same);

    /* --- wbudowany magazyn zaufania --- */
    int roots_ok = 0, roots_bad = 0;
    const uint8_t *p = ca_roots_der;
    uint32_t left = sizeof(ca_roots_der);
    while (left > 4 && p[0] == 0x30) {
        uint32_t l = p[1], rlen;
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

        struct x509_cert root;
        if (x509_parse(&root, p, rlen) == 0 && root.is_ca &&
            root.not_after[0])
            roots_ok++;
        else
            roots_bad++;
        p += rlen;
        left -= rlen;
    }
    print("      wbudowanych korzeni CA: ");
    print_dec((uint32_t)roots_ok);
    print("\n");
    result("wszystkie wbudowane korzenie daja sie rozczytac",
           roots_bad == 0 && roots_ok == CA_ROOTS_COUNT);

    if (failures) {
        print("[cert] BLEDOW: ");
        print_dec((uint32_t)failures);
        print("\n");
        sys_exit(1);
    }
    print("[cert] wszystko sie zgadza\n");
    sys_exit(0);
}
