#!/usr/bin/env python3
"""Test dymny ProtonOS: TLS 1.3 i klient HTTP w ring 3 (jadro 64-bitowe).

Kryptografia, TLS, HTTP, DNS i parser certyfikatow zyja w naglowkach
w user/ i nie wiedza nic o trybie procesora - licza na bajtach. Do
jadra mowia kilkoma nazwami i te nazwy sa cala roznica: przeklada je
user64/uzgodnosc64.h, dokladnie tak samo, jak compat64.c przeklada
slownik jadra.

Po drugiej stronie kabla stoi prawdziwy OpenSSL, a nie nasza wlasna
petla - wiec to, ze uzgodnienie dochodzi do skutku, znaczy, ze zgadza
sie kazdy bajt: X25519, ChaCha20-Poly1305, wyprowadzanie kluczy
i podpis w Finished.

Sprawdzamy tez to, co wazniejsze od samego szyfrowania: obcy
certyfikat ma polaczenie ZERWAC. Szyfrowana rozmowa z kims nieznanym
nie jest bezpieczna, tylko prywatna.
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
from ptest import check, wynik, zrob_iso64, brak64


def main():
    print("--- TLS 1.3 i HTTP z pierscienia 3 (jadro 64-bitowe) ---")

    powod = brak64()
    if powod:
        print("POMINIETE: " + powod)
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        check("plyta z jadrem 64-bitowym buduje sie", zrob_iso64(d, iso))
        if not os.path.exists(iso):
            return wynik()

        http_port, http_stop = start_http_server()
        https_port, https_stop = start_https_server()
        dns_stop = start_dns_server()
        if https_port is None:
            print("POMINIETE: brak openssl")
            return wynik()

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=90)
            q.wait_serial("DHCP: przyznano", timeout=30)

            # --- Zwykly HTTP: TCP z pierscienia 3 ---
            out = q.polecenie("run httpget 10.0.2.2 %d /" % http_port,
                              "kod wyjscia", timeout=60)
            check("TCP z ring 3: uzgodnienie i odpowiedz HTTP 200",
                  "[httpget] HTTP 200" in out)
            check("tresc strony dotarla w calosci",
                  "ProtonOS TCP dziala" in out)

            # --- DNS: nazwa zamiast adresu ---
            q.polecenie("dns 10.0.2.2", "serwer DNS ustawiony")
            nsl = q.polecenie("run nslookup protonos.test", "kod wyjscia",
                              timeout=40)
            check("DNS z ring 3: nazwa zamieniona na adres",
                  "10.0.2.2" in nsl)

            # --- TLS bez magazynu zaufania: polaczenie ma padac ---
            odm = q.polecenie("run httpget https://protonos.test %d /"
                              % https_port, "kod wyjscia", timeout=90)
            check("obcy certyfikat zrywa polaczenie",
                  "brak zaufanego wystawcy" in odm)

            # --- TLS z pominieciem sprawdzania (-k) ---
            sec = q.polecenie("run httpget -k https://protonos.test %d /"
                              % https_port, "kod wyjscia", timeout=90)
            check("TLS 1.3: uzgodnienie z serwerem OpenSSL",
                  "[httpget] TLS 1.3" in sec)
            check("HTTPS: odpowiedz 200 z pelna trescia",
                  "HTTP 200, tresc %d B" % len(PAGE) in sec)
            check("HTTPS: tresc odszyfrowana poprawnie",
                  "ProtonOS TCP dziala" in sec)
            check("-k mowi wprost, ze tozsamosc nie jest sprawdzona",
                  "certyfikat NIESPRAWDZONY" in sec)

            # --- Magazyn zaufania z dysku i pelna weryfikacja ---
            q.polecenie("run httpget 10.0.2.2 %d /cert.der roots.der"
                        % http_port, "kod wyjscia", timeout=60)
            spis = q.polecenie("fls", "roots.der")
            check("magazyn zaufania zapisany na dysku PFS",
                  "roots.der" in spis)

            ver = q.polecenie("run httpget https://protonos.test %d /"
                              % https_port, "kod wyjscia", timeout=90)
            check("certyfikat sprawdzony wzgledem magazynu z dysku",
                  "certyfikat sprawdzony" in ver)
            check("tresc pobrana po zweryfikowanym TLS",
                  "HTTP 200, tresc %d B" % len(PAGE) in ver)

            # --- Kryptografia i X.509 same w sobie ---
            kr = q.polecenie("run cryptotest", "kod wyjscia", timeout=120)
            check("wektory z RFC: SHA-256, HMAC, HKDF, ChaCha20, X25519",
                  "wszystkie wektory zgadzaja sie" in kr)
            ce = q.polecenie("run certtest", "kod wyjscia", timeout=120)
            check("X.509: RSA, PSS, ECDSA P-256 i P-384, SubjectAltName",
                  "[cert] wszystko sie zgadza" in ce)
            # Stos programu urosl w trakcie sam z siebie - weryfikacja
            # podpisu siega glebiej, niz mozna zgadnac z gory
            m = re.search(r"certtest\] koniec, kod wyjscia 0 "
                          r"\(ramek \d+ -> \d+, stron sterty \d+, "
                          r"stosu (\d+)\)", ce)
            check("stos programu urosl na zadanie w trakcie liczenia",
                  m is not None and int(m.group(1)) > 0)
        finally:
            q.close()
            if os.path.exists(slog):
                os.unlink(slog)

        # --- Przegladarka w oknie pulpitu: siec -> TLS -> GUI ---
        # Ostatnia martwa ikona pulpitu. Spina wszystko naraz:
        # klawiature i mysz z kompozytora, DNS, TCP, parser HTML
        # i rysowanie w oknie - a to wszystko z pierscienia 3.
        print("--- przegladarka w oknie (gweb) ---")
        slog2 = tempfile.mktemp(suffix=".log")
        q2 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog2, disk=disk,
                  sata=True, qemu="qemu-system-x86_64")
        try:
            q2.wait_serial("proton64>", timeout=90)
            q2.wait_serial("DHCP: przyznano", timeout=30)
            # Serwer nazw trzeba ustawic PRZED pulpitem: potem klawiatura
            # nalezy juz do kompozytora, a nie do shella jadra
            q2.polecenie("dns 10.0.2.2", "serwer DNS ustawiony")
            q2.type_line("pulpit")
            q2.wait_serial("Pulpit startuje", timeout=30)
            check("pulpit wstaje przed przegladarka", q2.wait_desktop(30))

            q2.mouse_home()
            q2.mouse_goto(24, 232)          # ikona 'Web' pod (8,220)
            q2.mouse_click()

            # Okno przegladarki: slot 2, tresc 320x200 od (95,65)
            def bialy_tekst():
                _, _, a = q2.screen_raw()
                return sum(1 for y in range(100, 250)
                           for x in range(100, 400)
                           if a(x, y) == (255, 255, 255))

            time.sleep(3)
            przed = bialy_tekst()

            # Klik w okno daje mu fokus klawiatury, potem kursor na bok
            q2.mouse_goto(150, 120)
            q2.mouse_click()
            q2.mouse_goto(500, 350)

            for _ in range(24):             # skasuj domyslny adres
                q2.sendkey("backspace")
            for ch in "protonos.test:%d/" % http_port:
                q2.sendkey(Qemu.KEYNAMES.get(ch, ch))
            q2.sendkey("ret")

            pokazala = False
            koniec = time.time() + 30
            while time.time() < koniec:
                if bialy_tekst() > przed + 50:
                    pokazala = True
                    break
                time.sleep(0.5)
            check("przegladarka rozwiazala nazwe, pobrala strone "
                  "i narysowala tekst", pokazala)
        finally:
            q2.close()
            if os.path.exists(slog2):
                os.unlink(slog2)
    finally:
        for stop in (http_stop, https_stop, dns_stop):
            try:
                stop()
            except Exception:
                pass
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, disk):
            if os.path.exists(f):
                os.unlink(f)

    return wynik()


if __name__ == "__main__":
    sys.exit(main())
