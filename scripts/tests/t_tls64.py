#!/usr/bin/env python3
"""ProtonOS smoke test: TLS 1.3 and the HTTP client in ring 3 (64-bit
kernel).

The cryptography, TLS, HTTP, DNS and the certificate parser live in the
headers in user/ and know nothing about the processor mode - they
compute on bytes. They talk to the kernel through a handful of names and
those names are the whole difference: user64/compat64.h translates them,
exactly as compat64.c translates the kernel's dictionary.

On the other side of the wire stands a real OpenSSL, not a loop of our
own - so a handshake reaching completion means every byte matches:
X25519, ChaCha20-Poly1305, key derivation and the signature in Finished.

We also check what matters more than the encryption itself: a foreign
certificate is to TEAR the connection DOWN. An encrypted conversation
with somebody unknown is not secure, only private.
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
from ptest import check, summary, make_iso64, no64, start_suite_server


def main():
    print("--- TLS 1.3 i HTTP z pierscienia 3 (jadro 64-bitowe) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        http_port, http_stop = start_http_server()
        https_port, https_stop = start_https_server()
        dns_stop = start_dns_server()
        if https_port is None:
            print("POMINIETE: brak openssl")
            return summary()

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=90)
            q.wait_serial("DHCP: przyznano", timeout=30)

            # --- Plain HTTP: TCP from ring 3 ---
            out = q.command("run httpget 10.0.2.2 %d /" % http_port,
                              "kod wyjscia", timeout=60)
            check("TCP z ring 3: uzgodnienie i odpowiedz HTTP 200",
                  "[httpget] HTTP 200" in out)
            check("tresc strony dotarla w calosci",
                  "ProtonOS TCP dziala" in out)

            # --- DNS: a name instead of an address ---
            q.command("dns 10.0.2.2", "serwer DNS ustawiony")
            nsl = q.command("run nslookup protonos.test", "kod wyjscia",
                              timeout=40)
            check("DNS z ring 3: nazwa zamieniona na adres",
                  "10.0.2.2" in nsl)

            # --- TLS with no trust store: the connection is to fail ---
            refusal = q.command("run httpget https://protonos.test %d /"
                              % https_port, "kod wyjscia", timeout=90)
            check("obcy certyfikat zrywa polaczenie",
                  "brak zaufanego wystawcy" in refusal)

            # --- TLS with the check skipped (-k) ---
            sec = q.command("run httpget -k https://protonos.test %d /"
                              % https_port, "kod wyjscia", timeout=90)
            check("TLS 1.3: uzgodnienie z serwerem OpenSSL",
                  "[httpget] TLS 1.3" in sec)
            check("HTTPS: odpowiedz 200 z pelna trescia",
                  "HTTP 200, tresc %d B" % len(PAGE) in sec)
            check("HTTPS: tresc odszyfrowana poprawnie",
                  "ProtonOS TCP dziala" in sec)
            check("-k mowi wprost, ze tozsamosc nie jest sprawdzona",
                  "certyfikat NIESPRAWDZONY" in sec)

            # --- A trust store from the disk and full verification ---
            q.command("run httpget 10.0.2.2 %d /cert.der roots.der"
                        % http_port, "kod wyjscia", timeout=60)
            listing = q.command("fls", "roots.der")
            check("magazyn zaufania zapisany na dysku PFS",
                  "roots.der" in listing)

            ver = q.command("run httpget https://protonos.test %d /"
                              % https_port, "kod wyjscia", timeout=90)
            check("certyfikat sprawdzony wzgledem magazynu z dysku",
                  "certyfikat sprawdzony" in ver)
            check("tresc pobrana po zweryfikowanym TLS",
                  "HTTP 200, tresc %d B" % len(PAGE) in ver)

            # --- The cryptography and X.509 in themselves ---
            kr = q.command("run cryptotest", "kod wyjscia", timeout=120)
            check("wektory z RFC: SHA-256, HMAC, HKDF, ChaCha20, X25519",
                  "wszystkie wektory zgadzaja sie" in kr)
            ce = q.command("run certtest", "kod wyjscia", timeout=120)
            check("X.509: RSA, PSS, ECDSA P-256 i P-384, SubjectAltName",
                  "[cert] wszystko sie zgadza" in ce)
            # The program's stack grew of its own accord along the way -
            # verifying a signature reaches deeper than can be guessed up
            # front
            m = re.search(r"certtest\] koniec, kod wyjscia 0 "
                          r"\(ramek \d+ -> \d+, stron sterty \d+, "
                          r"stosu (\d+)", ce)
            check("stos programu urosl na zadanie w trakcie liczenia",
                  m is not None and int(m.group(1)) > 0)

            # --- A server that will agree to ONE suite -----------------
            # Until now the client offered ChaCha20 alone, so "it
            # worked" said nothing about AES. These two servers will
            # agree to one suite each and refuse the handshake
            # otherwise - so a reply that arrives is proof of which
            # cipher the record was encrypted with.
            for suite, label in (
                    ("TLS_AES_128_GCM_SHA256", "AES-128-GCM"),
                    ("TLS_CHACHA20_POLY1305_SHA256", "ChaCha20-Poly1305")):
                sdir = tempfile.mkdtemp()
                with open(os.path.join(sdir, "index.html"), "wb") as fh:
                    fh.write(PAGE)
                sport, sstop, _ = start_suite_server(suite, sdir)
                if sport is None:
                    print("POMINIETE: brak openssl s_server")
                    break
                try:
                    got = q.command("run httpget -k https://protonos.test "
                                    "%d /index.html" % sport,
                                    "kod wyjscia", timeout=90)
                    check("serwer wylacznie na %s: uzgodnienie przechodzi"
                          % label, "[httpget] TLS 1.3" in got)
                    check("i tresc wraca odszyfrowana (%s)" % label,
                          "ProtonOS TCP dziala" in got)
                finally:
                    sstop()
                    shutil.rmtree(sdir, ignore_errors=True)
        finally:
            q.close()
            if os.path.exists(slog):
                os.unlink(slog)

        # --- The browser in a desktop window: network -> TLS -> GUI ---
        # The last dead icon on the desktop. It ties everything together
        # at once: the keyboard and mouse from the compositor, DNS, TCP,
        # the HTML parser and drawing in a window - and all of it from
        # ring 3.
        print("--- przegladarka w oknie (gweb) ---")
        slog2 = tempfile.mktemp(suffix=".log")
        q2 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog2, disk=disk,
                  sata=True, qemu="qemu-system-x86_64")
        try:
            q2.wait_serial("proton64>", timeout=90)
            q2.wait_serial("DHCP: przyznano", timeout=30)
            # The name server has to be set BEFORE the desktop: afterwards
            # the keyboard belongs to the compositor, not to the kernel's
            # shell
            q2.command("dns 10.0.2.2", "serwer DNS ustawiony")
            q2.type_line("pulpit")
            q2.wait_serial("Pulpit startuje", timeout=30)
            check("pulpit wstaje przed przegladarka", q2.wait_desktop(30))

            q2.mouse_home()
            q2.mouse_goto(24, 232)          # the 'Web' icon at (8,220)
            q2.mouse_click()

            # The browser's window: slot 2, content 320x200 from (95,65)
            def white_text():
                _, _, a = q2.screen_raw()
                return sum(1 for y in range(100, 250)
                           for x in range(100, 400)
                           if a(x, y) == (0, 0, 0))

            time.sleep(3)
            before = white_text()

            # A click in the window gives it keyboard focus, then the cursor
# goes aside
            q2.mouse_goto(150, 120)
            q2.mouse_click()
            q2.mouse_goto(500, 350)

            for _ in range(24):             # clear the default address
                q2.sendkey("backspace")
            for ch in "protonos.test:%d/" % http_port:
                q2.sendkey(Qemu.KEYNAMES.get(ch, ch))
            q2.sendkey("ret")

            showed = False
            end = time.time() + 30
            while time.time() < end:
                if white_text() > before + 50:
                    showed = True
                    break
                time.sleep(0.5)
            check("przegladarka rozwiazala nazwe, pobrala strone "
                  "i narysowala tekst", showed)
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

    return summary()


if __name__ == "__main__":
    sys.exit(main())
