#!/usr/bin/env python3
"""ProtonOS smoke test: TLS 1.3 in ring 3 (32-bit kernel).

Split out of the 'rdzen2' group, because TLS handshakes are the most
expensive thing in the whole set: each of them computes X25519 and
checks an RSA or ECDSA signature in software, and we do a dozen or so of
them. A separate machine computes that in parallel with the rest instead
of lengthening one long run.

On the other side of the wire stands a real OpenSSL, so a handshake
reaching completion means every byte matches. We also check something
more important than the encryption itself: a foreign certificate is to
TEAR the connection DOWN - an encrypted conversation with somebody
unknown is not secure, only private.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, DNS_HINT


def main():
    # The disk is needed here for the trust store: we fetch the issuer's
    # certificate over HTTP and write it to PFS, and only then check the
    # server's identity with it
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        q.wait_for("ProtonOS shell", timeout=40)
        q.wait_for("[siec] DHCP", timeout=30)
        time.sleep(0.5)

        # An ordinary HTTP server on the host - this is where the
        # certificates put into the trust store come from
        http_port, http_stop = start_http_server()
        try:
            # HTTPS: a full TLS 1.3 handshake with a real OpenSSL on the other
            # side (X25519 + ChaCha20-Poly1305, a verified Finished)
            https_port, https_stop = start_https_server()
            if https_port is None:
                print("INFO: brak openssl - pomijam testy TLS")
            else:
                dns_stop, dns_ok = start_dns_server()
                if not dns_ok:
                    # Everything below reaches the server by NAME, so
                    # with no name server of ours there is nothing here
                    # left to check.
                    print("POMINIETE (TLS): " + DNS_HINT)
                    https_stop()
                    return summary()
                try:
                    q.type_line("dns 10.0.2.2")
                    q.wait_for("serwer DNS ustawiony", timeout=10)

                    # 1. A self-signed certificate, and in the system only the
                    #    built-in public CA roots: the connection is to be
                    #    torn down
                    q.type_line("clear")
                    q.type_line("run httpget https://protonos.test %d /"
                                % https_port)
                    reg = q.wait_for("nie udalo sie polaczyc", timeout=60)
                    check("obcy certyfikat odrzucony przez wbudowany "
                          "magazyn zaufania",
                          "brak zaufanego wystawcy" in reg)

                    # ... unless we deliberately skip the check (-k)
                    q.type_line("clear")
                    q.type_line("run httpget -k https://protonos.test %d /"
                                % https_port)
                    sec = q.wait_for("tresc ", timeout=60)
                    check("TLS 1.3: uzgodnienie z serwerem OpenSSL",
                          "[httpget] TLS 1.3" in sec)
                    check("HTTPS: odpowiedz 200 z pelna trescia",
                          "HTTP 200, tresc %d B" % len(PAGE) in sec)
                    check("HTTPS: tresc odszyfrowana poprawnie",
                          "ProtonOS TCP dziala" in sec)
                    check("-k mowi wprost, ze tozsamosc nie jest sprawdzona",
                          "certyfikat NIESPRAWDZONY" in sec)

                    # 2. Fetching the issuer's certificate straight to the disk
                    q.type_line("clear")
                    q.type_line("run httpget 10.0.2.2 %d /cert.der roots.der"
                                % http_port)
                    q.wait_for("tresc ", timeout=30)
                    q.type_line("fls")
                    check("magazyn zaufania zapisany na dysku",
                          "roots.der" in q.wait_for("roots.der", timeout=10))

                    # 3. With the store: full verification of the chain, the
                    #    dates and the name
                    q.type_line("clear")
                    q.type_line("run httpget https://protonos.test %d /"
                                % https_port)
                    ver = q.wait_for("tresc ", timeout=60)
                    if "certyfikat sprawdzony" not in ver:
                        print("--- ekran (diagnostyka) ---")
                        print(ver)
                        print("---------------------------")
                    check("certyfikat sprawdzony wzgledem magazynu zaufania",
                          "certyfikat sprawdzony" in ver)
                    check("tresc pobrana po zweryfikowanym TLS",
                          "HTTP 200, tresc %d B" % len(PAGE) in ver)

                    # 4. A server with a certificate on the P-256 curve: the
                    #    whole chain and CertificateVerify go through
                    #    ECDSA
                    ec_port, ec_stop = start_https_server(ec=True)
                    if ec_port is not None:
                        try:
                            q.type_line("clear")
                            q.type_line("run httpget 10.0.2.2 %d /eccert.der "
                                        "roots.der" % http_port)
                            q.wait_for("tresc ", timeout=30)
                            q.type_line("clear")
                            q.type_line("run httpget https://protonos.test %d /"
                                        % ec_port)
                            ecres = q.wait_for("tresc ", timeout=90)
                            check("certyfikat na krzywej P-256 sprawdzony "
                                  "(ECDSA)",
                                  "certyfikat sprawdzony" in ecres)
                            check("tresc pobrana po TLS z kluczem EC",
                                  "HTTP 200, tresc %d B" % len(PAGE) in ecres)
                        finally:
                            ec_stop()

                        # go back to the store with the RSA certificate
                        q.type_line("clear")
                        q.type_line("run httpget 10.0.2.2 %d /cert.der "
                                    "roots.der" % http_port)
                        q.wait_for("tresc ", timeout=30)

                    # 4b. The P-384 curve (SHA-384 signatures) - ISRG Root X2,
                    #     among others, is like that
                    p384_port, p384_stop = start_https_server(ec=True,
                                                              p384=True)
                    if p384_port is not None:
                        try:
                            q.type_line("clear")
                            q.type_line("run httpget 10.0.2.2 %d "
                                        "/p384cert.der roots.der" % http_port)
                            q.wait_for("tresc ", timeout=30)
                            q.type_line("clear")
                            q.type_line("run httpget https://protonos.test %d /"
                                        % p384_port)
                            p3 = q.wait_for("tresc ", timeout=120)
                            check("certyfikat na krzywej P-384 sprawdzony "
                                  "(ECDSA + SHA-384)",
                                  "certyfikat sprawdzony" in p3)
                        finally:
                            p384_stop()

                    # 5. A two-level chain: we trust only the root, so the
                    #    system has to go through the intermediate
                    ch_port, ch_stop = start_https_chain_server()
                    if ch_port is not None:
                        try:
                            q.type_line("clear")
                            q.type_line("run httpget 10.0.2.2 %d /rootca.der "
                                        "roots.der" % http_port)
                            q.wait_for("tresc ", timeout=30)
                            q.type_line("clear")
                            q.type_line("run httpget https://protonos.test %d /"
                                        % ch_port)
                            chres = q.wait_for("tresc ", timeout=90)
                            check("lancuch przez posrednika domkniety do "
                                  "zaufanego korzenia",
                                  "certyfikat sprawdzony" in chres)
                        finally:
                            ch_stop()

                        q.type_line("clear")
                        q.type_line("run httpget 10.0.2.2 %d /cert.der "
                                    "roots.der" % http_port)
                        q.wait_for("tresc ", timeout=30)

                    # 6. The same server at an IP address - the name does not
                    #    match, so the connection is to be torn down
                    q.type_line("clear")
                    q.type_line("run httpget https://10.0.2.2 %d /" % https_port)
                    bad = q.wait_for("nie udalo sie polaczyc", timeout=60)
                    check("zla nazwa w certyfikacie odrzucona",
                          "certyfikat nie obejmuje tej nazwy" in bad)
                finally:
                    https_stop()
                    dns_stop()
        finally:
            http_stop()
    finally:
        q.close()
        if os.path.exists(disk):
            os.unlink(disk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
