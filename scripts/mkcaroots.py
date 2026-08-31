#!/usr/bin/env python3
"""Builds user/caroots.h - the ProtonOS built-in trust store.

It takes the named CA roots from the system's bundle (by default
/etc/ssl/certs/ca-certificates.crt) and writes them as glued DER
certificates in a C header. Thanks to that HTTPS works right away,
without uploading anything to the system's disk.

Usage: python3 scripts/mkcaroots.py [bundle.crt] [output.h]
"""

import re
import subprocess
import sys

# Roots covering most public sites. Each of them has to be signed with
# an algorithm we know how to check (RSA or ECDSA P-256).
WANTED = [
    "CN = ISRG Root X1",                    # Let's Encrypt (RSA)
    "CN = ISRG Root X2",                    # Let's Encrypt (EC P-384 - a spare only)
    "CN = DigiCert Global Root CA",
    "CN = DigiCert Global Root G2",
    "CN = DigiCert Global Root G3",
    "CN = Baltimore CyberTrust Root",
    "CN = GlobalSign Root CA",
    "OU = GlobalSign Root CA - R3",
    "OU = GlobalSign Root CA - R6",
    "CN = USERTrust RSA Certification Authority",
    "CN = Go Daddy Root Certificate Authority - G2",
    "CN = Amazon Root CA 1",
    "CN = Certum Trusted Network CA",       # popular in Poland
]

BUNDLE = sys.argv[1] if len(sys.argv) > 1 else "/etc/ssl/certs/ca-certificates.crt"
OUT = sys.argv[2] if len(sys.argv) > 2 else "user/caroots.h"


def subject_of(pem):
    out = subprocess.run(["openssl", "x509", "-noout", "-subject"],
                         input=pem, capture_output=True, text=True)
    return out.stdout.strip()


def der_of(pem):
    out = subprocess.run(["openssl", "x509", "-outform", "DER"],
                         input=pem.encode(), capture_output=True)
    return out.stdout


def main():
    pems = re.findall(
        r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----",
        open(BUNDLE).read(), re.S)

    chosen = []
    for pem in pems:
        subj = subject_of(pem)
        for want in WANTED:
            if want in subj:
                chosen.append((want, subj, der_of(pem)))
                break

    if not chosen:
        print("nie znalazlem zadnego korzenia w", BUNDLE)
        return 1

    blob = b"".join(der for _, _, der in chosen)

    lines = ["/* The built-in trust store - A GENERATED FILE.",
             " *",
             " * Source: %s" % BUNDLE,
             " * To refresh: python3 scripts/mkcaroots.py",
             " *",
             " * The roots inside:"]
    for _, subj, der in chosen:
        cn = subj.replace("subject=", "")
        lines.append(" *   - %s (%u B)" % (cn, len(der)))
    lines += [" */", "", "#ifndef PROTONOS_CAROOTS_H",
              "#define PROTONOS_CAROOTS_H", "", '#include "usys.h"', "",
              "#define CA_ROOTS_COUNT %u" % len(chosen), "",
              "static const uint8_t ca_roots_der[] = {"]

    line = "    "
    for b in blob:
        line += "0x%02x," % b
        if len(line) > 72:
            lines.append(line)
            line = "    "
    if line.strip():
        lines.append(line)
    lines += ["};", "", "#endif", ""]

    open(OUT, "w").write("\n".join(lines))
    print("zapisano %s: %u korzeni, %u B" % (OUT, len(chosen), len(blob)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
