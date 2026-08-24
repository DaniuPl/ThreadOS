/* certtest - sprawdzanie certyfikatow X.509 i podpisow RSA
 *
 * Bierze prawdziwy certyfikat (wygenerowany openssl-em, wbudowany w
 * certdata.h), rozbiera go parserem DER i sprawdza jego wlasny podpis.
 * Certyfikat jest samopodpisany, wiec do weryfikacji wystarczy klucz,
 * ktory sam w sobie niesie - nie trzeba zadnego magazynu zaufania.
 *
 * Dodatkowo: podpis w schemacie PSS (tego wymaga TLS 1.3) oraz proba
 * podmiany bajtu - podpis musi wtedy odpasc.
 */

#include "usys.h"
#include "usha256.h"
#include "ursa.h"
#include "ux509.h"
#include "certdata.h"

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

    if (failures) {
        print("[cert] BLEDOW: ");
        print_dec((uint32_t)failures);
        print("\n");
        sys_exit(1);
    }
    print("[cert] wszystko sie zgadza\n");
    sys_exit(0);
}
