#!/usr/bin/env python3
"""ThreadOS smoke test: TLS 1.3 and the HTTP client in ring 3 (64-bit
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
from ptest import (check, summary, make_iso64, no64,
                   start_suite_server, start_tls12_server, DNS_HINT)
from ptest import _kernel_define as define


def main():
    print("--- TLS 1.3 and HTTP from ring 3 (the 64-bit kernel) ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        check("the disc with the 64-bit kernel builds", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        http_port, http_stop = start_http_server()
        https_port, https_stop = start_https_server()
        dns_stop, dns_ok = start_dns_server()
        if https_port is None:
            print("SKIPPED: no openssl")
            return summary()
        if not dns_ok:
            # Everything below reaches the server by NAME, so without our
            # own name server there is nothing here to check.
            print("SKIPPED: " + DNS_HINT)
            return summary()

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=90)
            q.wait_serial("DHCP: granted", timeout=30)

            # --- Plain HTTP: TCP from ring 3 ---
            out = q.command("run httpget 10.0.2.2 %d /" % http_port,
                              "exit code", timeout=60)
            check("TCP from ring 3: a handshake and an HTTP 200 answer",
                  "[httpget] HTTP 200" in out)
            check("the page's content arrived whole",
                  "ThreadOS TCP works" in out)

            # --- DNS: a name instead of an address ---
            q.command("dns 10.0.2.2", "DNS server set")
            nsl = q.command("run nslookup threados.test", "exit code",
                              timeout=40)
            check("DNS from ring 3: a name turned into an address",
                  "10.0.2.2" in nsl)

            # --- TLS with no trust store: the connection is to fail ---
            refusal = q.command("run httpget https://threados.test %d /"
                              % https_port, "exit code", timeout=90)
            check("a foreign certificate tears the connection down",
                  "no trusted issuer" in refusal)

            # --- TLS with the check skipped (-k) ---
            sec = q.command("run httpget -k https://threados.test %d /"
                              % https_port, "exit code", timeout=90)
            check("TLS 1.3: a handshake with an OpenSSL server",
                  "[httpget] TLS 1.3" in sec)
            check("HTTPS: a 200 answer with the whole content",
                  "HTTP 200, content %d B" % len(PAGE) in sec)
            check("HTTPS: the content was decrypted correctly",
                  "ThreadOS TCP works" in sec)
            check("-k says outright that nobody's identity is checked",
                  "certificate NOT CHECKED" in sec)

            # --- A trust store from the disk and full verification ---
            q.command("run httpget 10.0.2.2 %d /cert.der roots.der"
                        % http_port, "exit code", timeout=60)
            listing = q.command("fls", "roots.der")
            check("the trust store was written to the TFS disk",
                  "roots.der" in listing)

            ver = q.command("run httpget https://threados.test %d /"
                              % https_port, "exit code", timeout=90)
            check("the certificate was checked against the store from the disk",
                  "certificate checked" in ver)
            check("the content was fetched over a verified TLS",
                  "HTTP 200, content %d B" % len(PAGE) in ver)

            # --- The cryptography and X.509 in themselves ---
            kr = q.command("run cryptotest", "exit code", timeout=120)
            check("vectors from the RFCs: SHA-256, HMAC, HKDF, ChaCha20, X25519",
                  "every vector matches" in kr)
            ce = q.command("run certtest", "exit code", timeout=120)
            check("X.509: RSA, PSS, ECDSA P-256 and P-384, SubjectAltName",
                  "[cert] everything matches" in ce)
            # The program's stack grew of its own accord along the way -
            # verifying a signature reaches deeper than can be guessed up
            # front
            m = re.search(r"certtest\] done, exit code 0 "
                          r"\(frames \d+ -> \d+, heap pages \d+, "
                          r"stack (\d+)", ce)
            check("the program's stack grew on demand while it counted",
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
                    print("SKIPPED: no openssl s_server")
                    break
                try:
                    got = q.command("run httpget -k https://threados.test "
                                    "%d /index.html" % sport,
                                    "exit code", timeout=90)
                    check("a server on %s alone: the handshake goes through"
                          % label, "[httpget] TLS 1.3" in got)
                    check("and the content comes back decrypted (%s)" % label,
                          "ThreadOS TCP works" in got)
                finally:
                    sstop()
                    shutil.rmtree(sdir, ignore_errors=True)

            # --- TLS 1.2, for servers that have not moved on -----------
            # Each server speaks 1.2 alone with one cipher, so a page that
            # arrives proves the suite; its certificate is the guest's
            # one trust root, so "checked" proves the signature over the
            # server's key and the chain both held.
            # The key exchange on each curve a 1.2 server may pick.
            for cipher, ec, groups, label in (
                    ("ECDHE-RSA-CHACHA20-POLY1305", False, "X25519",
                     "RSA, ChaCha20-Poly1305, x25519"),
                    ("ECDHE-RSA-AES128-GCM-SHA256", False, "P-256",
                     "RSA, AES-128-GCM, P-256"),
                    ("ECDHE-ECDSA-CHACHA20-POLY1305", True, "P-384",
                     "ECDSA, ChaCha20-Poly1305, P-384"),
                    ("ECDHE-ECDSA-AES128-GCM-SHA256", True, "X25519",
                     "ECDSA, AES-128-GCM, x25519")):
                sdir = tempfile.mkdtemp()
                with open(os.path.join(sdir, "index.html"), "wb") as fh:
                    fh.write(PAGE)
                sport, sstop, der = start_tls12_server(cipher, ec, sdir,
                                                       groups)
                if sport is None:
                    print("SKIPPED: no openssl s_server")
                    break
                try:
                    SERVED["/tls12.der"] = der
                    q.command("run httpget 10.0.2.2 %d /tls12.der roots.der"
                              % http_port, "exit code", timeout=60)
                    got = q.command("run httpget https://threados.test "
                                    "%d /index.html" % sport,
                                    "exit code", timeout=120)
                    said = [ln for ln in got.splitlines()
                            if "[httpget]" in ln][-3:]
                    check("TLS 1.2 with %s: agreed, the certificate and the "
                          "signature over the key checked, the page whole "
                          "(%s)" % (label, said),
                          "[httpget] TLS 1.2 agreed" in got and
                          "certificate checked" in got and
                          "ThreadOS TCP works" in got)
                finally:
                    sstop()
                    shutil.rmtree(sdir, ignore_errors=True)
            # And a 1.2 server with a certificate that is not the root
            sdir = tempfile.mkdtemp()
            sport, sstop, _ = start_tls12_server(
                "ECDHE-RSA-AES128-GCM-SHA256", False, sdir)
            try:
                if sport is not None:
                    got = q.command("run httpget https://threados.test "
                                    "%d /index.html" % sport,
                                    "exit code", timeout=120)
                    check("a TLS 1.2 server with a foreign certificate is "
                          "refused", "no trusted issuer" in got and
                          "HTTP 200" not in got)
            finally:
                sstop()
                shutil.rmtree(sdir, ignore_errors=True)
        finally:
            q.close()
            if os.path.exists(slog):
                os.unlink(slog)

        # --- The browser in a desktop window: network -> TLS -> GUI ---
        # It ties everything together at once: the keyboard and mouse
        # from the compositor, DNS, TCP, the HTML parser and drawing in a
        # window - and all of it from ring 3.
        print("--- the browser in a window (web) ---")
        slog2 = tempfile.mktemp(suffix=".log")
        q2 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog2, disk=disk,
                  sata=True, qemu="qemu-system-x86_64")
        try:
            q2.wait_serial("thread64>", timeout=90)
            q2.wait_serial("DHCP: granted", timeout=30)
            # The name server has to be set BEFORE the desktop: afterwards
            # the keyboard belongs to the compositor, not to the kernel's
            # shell
            q2.command("dns 10.0.2.2", "DNS server set")
            q2.type_line("desktop")
            q2.wait_serial("The desktop is starting", timeout=30)
            check("the desktop comes up before the browser", q2.wait_desktop(30))

            src = "apps/browser/web.cpp"
            w0, h0 = define(src, "W0"), define(src, "H0")
            wx, wy = cascade(2, w0 + 2, h0 + TITLE_H + 2)
            cx, cy = wx + 1, wy + CONTENT_DY
            bar = define(src, "BAR_H")
            q2.mouse_home()
            q2.launch_dock("gweb")                # the "Web" tile
            end = time.time() + 30
            while time.time() < end and \
                    q2.window_title(wx, wy, w0 + 2) != "Web: Start":
                time.sleep(0.5)

            # A click picks the address whole; what is typed replaces it.
            # A bare host name, the way people type one.
            q2.click(cx + define(src, "ADDR_X") + 60, cy + bar // 2)
            q2.mouse_goto(600, 440)
            time.sleep(1)
            for ch in "threados.test:%d/" % http_port:
                q2.sendkey(Qemu.KEYNAMES.get(ch, ch))
            q2.sendkey("ret")

            # The server's page, read back off the window in the page's
            # own black on white
            said = ""
            end = time.time() + 40
            while time.time() < end and said != "ThreadOS TCP works":
                said = q2.read_page(cx + 2, cy + bar, cx + 300, cy + bar + 40,
                                    define(src, "FONT_PX"), (0, 0, 0),
                                    want="ThreadOS TCP works")
                time.sleep(0.5)
            check("the browser resolved the name, fetched the page "
                  "and drew its text (%r)" % said,
                  said == "ThreadOS TCP works")
            check("the bare name became an http address",
                  "[web] opened http://threados.test:%d/" % http_port
                  in q2.serial())
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
