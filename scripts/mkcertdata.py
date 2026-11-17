#!/usr/bin/env python3
"""Generates user/certdata.h - the fixture certtest checks itself against.

Everything in that header is DATA in the strict sense: a signature is
over exact bytes, so a single character changed by hand anywhere in it
turns a passing test into a failing one for a reason nobody will guess.
The translation and the rename both learned that the hard way - the
message under the PSS signature was edited twice, and twice the only
visible symptom was 'certtest: FAILURES: 2'.

So the fixture is generated rather than hand-kept, and this script is
how. It needs openssl on the host:

    python3 scripts/mkcertdata.py

What it makes, and why each piece is there:

  cert_der       RSA-2048, sha256WithRSAEncryption, self-signed, with a
                 SubjectAltName on threados.test and *.threados.test.
                 Self-signed on purpose: the key it carries is enough to
                 check its own signature, so no trust store is needed
                 and the DER parser gets exercised on the way.
  pss_sig        the same key signing MESSAGE in the PSS scheme, which
                 is what TLS 1.3 demands in CertificateVerify. The salt
                 is 32 bytes because that is what rsa_verify_pss_sha256
                 accepts - MGF1-SHA256, salt length equal to the digest.
  ec_cert_der    the same shape on the P-256 curve (ecdsa-with-SHA256).
  ec_sig         an ECDSA signature of MESSAGE under that key, as the
                 DER SEQUENCE { r, s } a TLS server would send.
  p384_cert_der  P-384 signed with SHA-384, the way ISRG Root X2 is.
  offcurve_*     a "public key" (x, 0) that is not on P-256 - on the
                 curve with the same p and a and another b it has order
                 2 - and an (r, s) that the verification formulas, which
                 never use b, accept under it for OFFCURVE_MESSAGE: u2
                 is even, so the key's share cancels out. Made here, by
                 this script's own arithmetic; certtest wants it
                 refused, which only the check that a key is on the
                 curve does.

The keys are thrown away with the temporary directory. Nothing here is
a secret worth keeping: the fixture only ever proves the code agrees
with itself and with openssl.
"""

import hashlib
import os
import random
import subprocess
import sys
import tempfile

MESSAGE = "ThreadOS test message for the PSS and ECDSA signatures"
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "user", "certdata.h")
NAME = "threados.test"
DAYS = "3650"


def run(*args, **kw):
    return subprocess.run(args, check=True, capture_output=True, **kw)


def c_array(name, data):
    out = ["static const uint8_t %s[] = {" % name]
    for i in range(0, len(data), 14):
        row = "".join("0x%02x," % b for b in data[i:i + 14])
        out.append("    " + row)
    out.append("};")
    return "\n".join(out)


# P-256 (FIPS 186-4, D.1.2.3)
P256_P = 0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff
P256_N = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551
P256_G = (0x6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296,
          0x4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5)
OFFCURVE_MESSAGE = b"ThreadOS: a key that is not on the curve"


def p256_add(a, b):
    p = P256_P
    if a is None:
        return b
    if b is None:
        return a
    if a[0] == b[0] and (a[1] + b[1]) % p == 0:
        return None
    if a == b:
        lam = (3 * a[0] * a[0] - 3) * pow(2 * a[1], -1, p) % p
    else:
        lam = (b[1] - a[1]) * pow(b[0] - a[0], -1, p) % p
    x = (lam * lam - a[0] - b[0]) % p
    return (x, (lam * (a[0] - x) - a[1]) % p)


def p256_mul(k, pt):
    out = None
    while k:
        if k & 1:
            out = p256_add(out, pt)
        pt = p256_add(pt, pt)
        k >>= 1
    return out


def offcurve():
    """(key, r, s) for OFFCURVE_MESSAGE as described above - seeded, so
    the header comes out the same every time"""
    rng = random.Random(20261117)
    e = int.from_bytes(hashlib.sha256(OFFCURVE_MESSAGE).digest(), "big")
    x0 = rng.randrange(1, P256_P)
    while True:
        s = rng.randrange(1, P256_N)
        w = pow(s, -1, P256_N)
        r = p256_mul(e * w % P256_N, P256_G)[0] % P256_N
        u2 = r * w % P256_N
        if r and u2 and u2 % 2 == 0:
            break
    key = b"\x04" + x0.to_bytes(32, "big") + bytes(32)
    return key, r.to_bytes(32, "big"), s.to_bytes(32, "big")


def main():
    d = tempfile.mkdtemp(prefix="threados-certdata-")
    msg = os.path.join(d, "msg.bin")
    with open(msg, "wb") as f:
        f.write(MESSAGE.encode())

    # --- RSA-2048, self-signed, with the names on it ------------------
    rsa_key = os.path.join(d, "rsa.key")
    rsa_crt = os.path.join(d, "rsa.der")
    run("openssl", "genrsa", "-out", rsa_key, "2048")
    run("openssl", "req", "-new", "-x509", "-key", rsa_key, "-sha256",
        "-days", DAYS, "-outform", "DER", "-out", rsa_crt,
        "-subj", "/CN=" + NAME,
        "-addext", "subjectAltName=DNS:%s,DNS:*.%s" % (NAME, NAME))

    # The salt has to be exactly the digest length: that is the one
    # length rsa_verify_pss_sha256 knows how to undo.
    pss = os.path.join(d, "pss.sig")
    run("openssl", "dgst", "-sha256",
        "-sigopt", "rsa_padding_mode:pss",
        "-sigopt", "rsa_pss_saltlen:32",
        "-sigopt", "rsa_mgf1_md:sha256",
        "-sign", rsa_key, "-out", pss, msg)

    # --- P-256: a certificate and a signature of the message ----------
    ec_key = os.path.join(d, "ec.key")
    ec_crt = os.path.join(d, "ec.der")
    run("openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout",
        "-out", ec_key)
    run("openssl", "req", "-new", "-x509", "-key", ec_key, "-sha256",
        "-days", DAYS, "-outform", "DER", "-out", ec_crt,
        "-subj", "/CN=" + NAME,
        "-addext", "subjectAltName=DNS:%s" % NAME)
    ec_sig = os.path.join(d, "ec.sig")
    run("openssl", "dgst", "-sha256", "-sign", ec_key, "-out", ec_sig, msg)

    # --- P-384, signed with SHA-384 -----------------------------------
    p384_key = os.path.join(d, "p384.key")
    p384_crt = os.path.join(d, "p384.der")
    run("openssl", "ecparam", "-name", "secp384r1", "-genkey", "-noout",
        "-out", p384_key)
    run("openssl", "req", "-new", "-x509", "-key", p384_key, "-sha384",
        "-days", DAYS, "-outform", "DER", "-out", p384_crt,
        "-subj", "/CN=" + NAME,
        "-addext", "subjectAltName=DNS:%s" % NAME)

    blobs = [("cert_der", rsa_crt), ("pss_sig", pss),
             ("ec_cert_der", ec_crt), ("ec_sig", ec_sig),
             ("p384_cert_der", p384_crt)]
    data = {n: open(p, "rb").read() for n, p in blobs}

    text = HEADER % (NAME, NAME)
    text += "\n" + c_array("cert_der", data["cert_der"]) + "\n"
    text += "\n" + c_array("pss_sig", data["pss_sig"]) + "\n"
    text += "\n" + c_array("ec_cert_der", data["ec_cert_der"]) + "\n"
    text += "\n" + c_array("ec_sig", data["ec_sig"]) + "\n"
    text += MSG_BLOCK % MESSAGE
    text += "\n" + c_array("p384_cert_der", data["p384_cert_der"]) + "\n"
    key, r, s = offcurve()
    text += OFFCURVE_BLOCK % OFFCURVE_MESSAGE.decode()
    text += "\n" + c_array("offcurve_key", key) + "\n"
    text += "\n" + c_array("offcurve_r", r) + "\n"
    text += "\n" + c_array("offcurve_s", s) + "\n"
    text += "\n#endif\n"
    with open(OUT, "w") as f:
        f.write(text)

    print("%s: %d B" % (OUT, len(text)))
    for n, _ in blobs:
        print("  %-14s %4d B" % (n, len(data[n])))


HEADER = '''/* Test data for certificate checking - GENERATED, do not edit by hand.
 *
 * Made by scripts/mkcertdata.py, which needs openssl:
 *
 *     python3 scripts/mkcertdata.py
 *
 * Every array here is bytes a signature was computed over, so editing
 * one by hand - even to correct a word in the message below - breaks
 * the test in a way that points nowhere near the edit. Regenerate
 * instead.
 *
 * cert_der: a self-signed certificate (RSA-2048,
 *   sha256WithRSAEncryption) with a SubjectAltName on %s and
 *   *.%s. Since its own key signed it, the signature can be
 *   checked without any trust store - and the whole DER parser along
 *   the way.
 * pss_sig: a signature over the message below in the PSS scheme (which
 *   TLS 1.3 requires in the CertificateVerify message).
 * ec_cert_der: the same, but on the P-256 curve (ecdsa-with-SHA256) -
 *   more and more servers issue exactly such certificates.
 * ec_sig: an ECDSA signature over the same message (DER: SEQUENCE
 *   { r, s }).
 * p384_cert_der: the P-384 curve signed with a SHA-384 digest - that is
 *   how ISRG Root X2, among others, is issued.
 */

#ifndef THREADOS_CERTDATA_H
#define THREADOS_CERTDATA_H

#include "uapi.h"
'''

MSG_BLOCK = '''
/* The message pss_sig and ec_sig were computed over. It is DATA, not a
   sentence anybody reads: both signatures are over these exact bytes,
   so a word changed here invalidates both. */
static const char pss_msg[] = "%s";
'''

OFFCURVE_BLOCK = '''
/* A key off the curve and a signature its formulas accept (see
   scripts/mkcertdata.py, offcurve) - over this message */
static const char offcurve_msg[] = "%s";
'''

main()
