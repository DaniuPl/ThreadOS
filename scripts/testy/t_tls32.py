#!/usr/bin/env python3
"""Test dymny ProtonOS: TLS 1.3 w pierscieniu 3 (jadro 32-bitowe).

Wydzielone z grupy 'rdzen2', bo uzgodnienia TLS sa najdrozsza rzecza
w calym zestawie: kazde z nich liczy X25519 i sprawdza podpis RSA albo
ECDSA programowo, a robimy ich kilkanascie. Osobna maszyna liczy to
rownolegle z reszta zamiast przedluzac jeden dlugi bieg.

Po drugiej stronie kabla stoi prawdziwy OpenSSL, wiec dojscie
uzgodnienia do skutku znaczy, ze zgadza sie kazdy bajt. Sprawdzamy tez
rzecz wazniejsza od samego szyfrowania: obcy certyfikat ma polaczenie
ZERWAC - szyfrowana rozmowa z kims nieznanym nie jest bezpieczna,
tylko prywatna.

Uruchamiany osobno albo przez scripts/run_testy.py, ktory puszcza
wszystkie grupy rownolegle.
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
from ptest import check, wynik


def main():
    # Dysk jest tu potrzebny na magazyn zaufania: certyfikat wystawcy
    # sciagamy po HTTP i zapisujemy na PFS, a dopiero potem sprawdzamy
    # nim tozsamosc serwera
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        q.wait_for("ProtonOS shell", timeout=40)
        q.wait_for("[siec] DHCP", timeout=30)
        time.sleep(0.5)

        # Zwykly serwer HTTP na hoscie - stad biora sie certyfikaty
        # wkladane do magazynu zaufania
        http_port, http_stop = start_http_server()
        try:
            # HTTPS: pelne uzgodnienie TLS 1.3 z prawdziwym OpenSSL-em po
            # drugiej stronie (X25519 + ChaCha20-Poly1305, sprawdzone Finished)
            https_port, https_stop = start_https_server()
            if https_port is None:
                print("INFO: brak openssl - pomijam testy TLS")
            else:
                dns_stop = start_dns_server()
                try:
                    q.type_line("dns 10.0.2.2")
                    q.wait_for("serwer DNS ustawiony", timeout=10)

                    # 1. Certyfikat samopodpisany, a w systemie tylko
                    #    wbudowane korzenie publicznych CA: polaczenie ma
                    #    zostac zerwane
                    q.type_line("clear")
                    q.type_line("run httpget https://protonos.test %d /"
                                % https_port)
                    rej = q.wait_for("nie udalo sie polaczyc", timeout=60)
                    check("obcy certyfikat odrzucony przez wbudowany "
                          "magazyn zaufania",
                          "brak zaufanego wystawcy" in rej)

                    # ... chyba ze swiadomie pominiemy sprawdzanie (-k)
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

                    # 2. Pobranie certyfikatu wystawcy prosto na dysk
                    q.type_line("clear")
                    q.type_line("run httpget 10.0.2.2 %d /cert.der roots.der"
                                % http_port)
                    q.wait_for("tresc ", timeout=30)
                    q.type_line("fls")
                    check("magazyn zaufania zapisany na dysku",
                          "roots.der" in q.wait_for("roots.der", timeout=10))

                    # 3. Z magazynem: pelna weryfikacja lancucha, dat i nazwy
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

                    # 4. Serwer z certyfikatem na krzywej P-256: caly
                    #    lancuch i CertificateVerify ida przez ECDSA
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

                        # wroc do magazynu z certyfikatem RSA
                        q.type_line("clear")
                        q.type_line("run httpget 10.0.2.2 %d /cert.der "
                                    "roots.der" % http_port)
                        q.wait_for("tresc ", timeout=30)

                    # 4b. Krzywa P-384 (podpisy SHA-384) - taka jest m.in.
                    #     ISRG Root X2
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

                    # 5. Lancuch dwupoziomowy: ufamy tylko korzeniowi,
                    #    wiec system musi przejsc przez posrednika
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

                    # 6. Ten sam serwer pod adresem IP - nazwa sie nie zgadza,
                    #    wiec polaczenie ma zostac zerwane
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

    return wynik()


if __name__ == "__main__":
    sys.exit(main())
