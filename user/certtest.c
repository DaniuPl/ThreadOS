/* certtest - checking X.509 certificates and RSA signatures
 *
 * It takes a real certificate (generated with openssl, built into
 * certdata.h), takes it apart with the DER parser and checks its own
 * signature. The certificate is self-signed, so the key it carries
 * itself is enough for verification - no trust store is needed.
 *
 * Additionally: a signature in the PSS scheme (which TLS 1.3
 * requires), the same for a certificate on the P-256 curve (ECDSA) and
 * an attempt to swap a byte - the signature then has to fall apart.
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
    print(ok ? ": OK\n" : ": FAILED\n");
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

    print("[cert] the X.509 parser and RSA signatures\n");

    result("taking a DER certificate apart",
           x509_parse(&cert, cert_der, sizeof(cert_der)) == 0);
    if (failures)
        sys_exit(1);

    print("      valid from ");
    print(cert.not_before);
    print(" do ");
    print(cert.not_after);
    print("\n");

    result("the RSA public key was found",
           cert.key_type == X509_KEY_RSA && cert.mod_len == 256);
    result("the public exponent is 65537", cert.exp == 65537);
    result("signature algorithm: RSA + SHA-256",
           cert.sig_alg == X509_ALG_RSA_SHA256);
    result("the signature length matches the key",
           cert.sig_len == cert.mod_len);

    /* The signature covers the tbsCertificate bytes */
    sha256(cert.tbs, cert.tbs_len, hash);
    result("the certificate's own signature (PKCS#1 v1.5)",
           rsa_verify_pkcs1_sha256(cert.mod, cert.mod_len, cert.exp,
                                   cert.sig, cert.sig_len, hash));

    /* The same verification after swapping a byte of the body has to fall
   apart */
    static uint8_t tampered[4096];
    for (uint32_t i = 0; i < sizeof(cert_der); i++)
        tampered[i] = cert_der[i];
    tampered[cert.tbs_len / 2] ^= 0x01;

    struct x509_cert bad;
    if (x509_parse(&bad, tampered, sizeof(cert_der)) == 0) {
        sha256(bad.tbs, bad.tbs_len, hash);
        result("changed content refused",
               !rsa_verify_pkcs1_sha256(bad.mod, bad.mod_len, bad.exp,
                                        bad.sig, bad.sig_len, hash));
    } else {
        result("changed content refused (in the parser already)", 1);
    }

    /* A PSS signature - that is what TLS 1.3 requires in
   CertificateVerify */
    uint32_t msglen = 0;
    while (pss_msg[msglen])
        msglen++;
    sha256(pss_msg, msglen, hash);
    result("a signature in the PSS scheme",
           rsa_verify_pss_sha256(cert.mod, cert.mod_len, cert.exp,
                                 pss_sig, sizeof(pss_sig), hash, 2048));

    hash[0] ^= 0x01;
    result("PSS: a wrong digest refused",
           !rsa_verify_pss_sha256(cert.mod, cert.mod_len, cert.exp,
                                  pss_sig, sizeof(pss_sig), hash, 2048));

    /* A host name match from SubjectAltName, or the lack of one */
    if (cert.san) {
        result("SubjectAltName: 'threados.test' matches",
               x509_matches_host(&cert, "threados.test"));
        result("SubjectAltName: a foreign name refused",
               !x509_matches_host(&cert, "wrong.example"));
    } else {
        print_str("      ", "a certificate with no SubjectAltName (the old style)");
    }

    /* --- a certificate on the P-256 curve --- */
    struct x509_cert ec;
    result("taking an ECDSA certificate apart",
           x509_parse(&ec, ec_cert_der, sizeof(ec_cert_der)) == 0);
    result("a key on the P-256 curve recognised",
           ec.key_type == X509_KEY_EC && ec.ec_bits == 256 &&
           ec.ec_point_len == 65);
    result("signature algorithm: ECDSA + SHA-256",
           ec.sig_alg == X509_ALG_ECDSA_SHA256);

    uint32_t t0, t1;
    t0 = sys_getticks();
    sha256(ec.tbs, ec.tbs_len, hash);
    result("the certificate's own signature (ECDSA P-256)",
           ecdsa_verify_der(ec_curve_p256(), ec.ec_point, ec.ec_point_len, hash, 32,
                            ec.sig, ec.sig_len));
    t1 = sys_getticks();
    print("      checking the ECDSA signature took ~");
    print_dec((t1 - t0) * 10);
    print(" ms\n");

    hash[0] ^= 0x01;
    result("ECDSA: a wrong digest refused",
           !ecdsa_verify_der(ec_curve_p256(), ec.ec_point, ec.ec_point_len, hash, 32,
                             ec.sig, ec.sig_len));

    sha256(pss_msg, msglen, hash);
    result("an ECDSA signature of a message (a separate vector)",
           ecdsa_verify_der(ec_curve_p256(), ec.ec_point, ec.ec_point_len, hash, 32,
                            ec_sig, sizeof(ec_sig)));

    /* --- a certificate on the P-384 curve (SHA-384) --- */
    struct x509_cert ec384;
    uint8_t hash384[48];

    result("taking a P-384 certificate apart",
           x509_parse(&ec384, p384_cert_der, sizeof(p384_cert_der)) == 0);
    result("a key on the P-384 curve recognised",
           ec384.key_type == X509_KEY_EC && ec384.ec_bits == 384 &&
           ec384.ec_point_len == 97);
    result("signature algorithm: ECDSA + SHA-384",
           ec384.sig_alg == X509_ALG_ECDSA_SHA384);

    t0 = sys_getticks();
    sha384(ec384.tbs, ec384.tbs_len, hash384);
    result("the certificate's own signature (ECDSA P-384)",
           ecdsa_verify_der(ec_curve_p384(), ec384.ec_point, ec384.ec_point_len,
                            hash384, 48, ec384.sig, ec384.sig_len));
    t1 = sys_getticks();
    print("      checking the P-384 signature took ~");
    print_dec((t1 - t0) * 10);
    print(" ms\n");

    hash384[0] ^= 0x01;
    result("P-384: a wrong digest refused",
           !ecdsa_verify_der(ec_curve_p384(), ec384.ec_point, ec384.ec_point_len,
                             hash384, 48, ec384.sig, ec384.sig_len));

    /* SHA-384 on a known vector (FIPS 180-4) */
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
    result("SHA-384('abc') matches FIPS 180-4", same);

    /* --- the built-in trust store --- */
    int roots_ok = 0, roots_bad = 0;
    uint32_t left = 0;
    const uint8_t *p = cert_builtin_roots(&left);
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
    print("      built-in CA roots: ");
    print_dec((uint32_t)roots_ok);
    print("\n");
    result("every built-in root can be read",
           roots_bad == 0 && roots_ok == CA_ROOTS_COUNT);

    if (failures) {
        print("[cert] FAILURES: ");
        print_dec((uint32_t)failures);
        print("\n");
        sys_exit(1);
    }
    print("[cert] everything matches\n");
    sys_exit(0);
}
