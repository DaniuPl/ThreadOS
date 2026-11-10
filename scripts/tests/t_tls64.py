#!/usr/bin/env python3
"""ThreadOS smoke test: TLS 1.3 and 1.2 and the HTTP client in ring 3.

The cryptography, TLS, HTTP, DNS and the certificate parser live in the
headers in user/ and compute on bytes. They talk to the kernel through
a handful of names, which user64/compat64.h maps onto the system calls.

On the other side of the wire stands a real OpenSSL, not a loop of our
own - so a handshake reaching completion means every byte matches:
X25519, ChaCha20-Poly1305, key derivation and the signature in Finished.
The server's certificate is signed with RSA, on the P-256 and P-384
curves, or through an intermediate; each has to check out against the
trust store on the disk.

We also check what matters more than the encryption itself: a foreign
certificate, or one issued for another name, is to TEAR the connection
DOWN. An encrypted conversation with somebody unknown is not secure,
only private.
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

# The shell's line once a program has ended, whole - up to its closing
# parenthesis. Waiting for less ('exit code') can hand back a line the
# serial file holds only half of.
DONE_LINE = re.compile(r"\] done, exit code -?\d+ \([^)\n]*\)")


def run(q, cmd, timeout):
    """Runs a program from the kernel's shell and hands back what arrived
    on the serial port up to the shell's complete 'done' line (or until
    the timeout). Everything the program printed comes before it."""
    start = len(q.serial())
    q.type_line(cmd)
    end = time.time() + timeout
    while time.time() < end:
        fresh = q.serial()[start:]
        if DONE_LINE.search(fresh):
            return fresh
        time.sleep(0.3)
    return q.serial()[start:]


def main():
    print("--- TLS 1.3 and HTTP from ring 3 ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    http_stop = https_stop = dns_stop = (lambda: None)
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
        # Each part asks for what it needs and no more. The plain fetch
        # and the cryptography's own vectors need neither; the name
        # lookup and the browser need our name server; TLS needs both
        # OpenSSL on the other end and the name, which its certificates
        # are issued for. What cannot run is said at the start and
        # again at the end, where a reader of the result looks.
        skipped = []
        if not dns_ok:
            skipped.append("the name lookup, TLS and the browser: "
                           + DNS_HINT)
        elif https_port is None:
            skipped.append("TLS: no openssl")
        for why in skipped:
            print("SKIPPED (" + why + ")")
        tls = dns_ok and https_port is not None

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=90)
            q.wait_serial("DHCP: granted", timeout=30)

            # --- Plain HTTP: TCP from ring 3 ---
            out = run(q, "run httpget 10.0.2.2 %d /" % http_port, 60)
            check("TCP from ring 3: a handshake and an HTTP 200 answer",
                  "[httpget] HTTP 200" in out)
            check("the page's content arrived whole",
                  "ThreadOS TCP works" in out)

            # --- The cryptography and X.509 in themselves ---
            kr = run(q, "run cryptotest", 120)
            check("vectors from the RFCs: SHA-256, HMAC, HKDF, ChaCha20, X25519",
                  "every vector matches" in kr)
            ce = run(q, "run certtest", 120)
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

            # --- DNS: a name instead of an address ---
            if dns_ok:
                q.command("dns 10.0.2.2", "DNS server set")
                nsl = run(q, "run nslookup threados.test", 40)
                # The whole answer line: the question's own echo names
                # the server (10.0.2.2) too, so the address alone proves
                # nothing
                check("DNS from ring 3: the server named with 'dns' turned "
                      "the name into an address",
                      "threados.test has the address 10.0.2.2" in nsl)

            if tls:
                tls_checks(q, http_port, https_port)
        finally:
            q.close()
            if os.path.exists(slog):
                os.unlink(slog)

        if dns_ok:
            browser(iso, disk, http_port)
        for why in skipped:
            print("SKIPPED (" + why + ")")
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


def tls_checks(q, http_port, https_port):
    """Everything over TLS: OpenSSL on the other end, reached by name."""
    # --- TLS with no trust store: the connection is to fail ---
    refusal = run(q, "run httpget https://threados.test %d /"
                  % https_port, 90)
    check("a foreign certificate tears the connection down",
          "no trusted issuer" in refusal)

    # --- TLS with the check skipped (-k) ---
    sec = run(q, "run httpget -k https://threados.test %d /"
              % https_port, 90)
    check("TLS 1.3: a handshake with an OpenSSL server",
          "[httpget] TLS 1.3" in sec)
    check("HTTPS: a 200 answer with the whole content",
          "HTTP 200, content %d B" % len(PAGE) in sec)
    check("HTTPS: the content was decrypted correctly",
          "ThreadOS TCP works" in sec)
    check("-k says outright that nobody's identity is checked",
          "certificate NOT CHECKED" in sec)

    # --- A trust store from the disk and full verification ---
    run(q, "run httpget 10.0.2.2 %d /cert.der roots.der"
        % http_port, 60)
    listing = q.command("fls", "roots.der")
    check("the trust store was written to the TFS disk",
          "roots.der" in listing)

    ver = run(q, "run httpget https://threados.test %d /"
              % https_port, 90)
    check("the certificate was checked against the store from the disk",
          "certificate checked" in ver)
    check("the content was fetched over a verified TLS",
          "HTTP 200, content %d B" % len(PAGE) in ver)

    # --- The same server at its IP address ---
    # Its certificate is trusted now, so the one thing wrong is
    # the name: it was issued for threados.test, not 10.0.2.2
    bad = run(q, "run httpget https://10.0.2.2 %d /" % https_port, 90)
    check("a wrong name in the certificate is refused",
          "the certificate does not cover this name" in bad and
          "HTTP 200" not in bad)

    # --- Certificates on the NIST curves, over TLS 1.3 ---
    # The certificate's signature and CertificateVerify both go
    # through ECDSA: P-256 with SHA-256, and P-384 with SHA-384 -
    # ISRG Root X2, among others, is like that. The issuer's
    # certificate is the trust store, one at a time.
    for kw, der, curve, how in (
            ({"ec": True}, "/eccert.der", "P-256", "ECDSA"),
            ({"ec": True, "p384": True}, "/p384cert.der", "P-384",
             "ECDSA + SHA-384")):
        ec_port, ec_stop = start_https_server(**kw)
        if ec_port is None:
            print("SKIPPED (%s): no openssl" % curve)
            continue
        try:
            run(q, "run httpget 10.0.2.2 %d %s roots.der"
                % (http_port, der), 60)
            got = run(q, "run httpget https://threados.test %d /"
                      % ec_port, 120)
            check("a certificate on the %s curve checked over "
                  "TLS 1.3 (%s)" % (curve, how),
                  "[httpget] TLS 1.3 agreed" in got and
                  "certificate checked" in got)
            check("the content was fetched over TLS with a %s key"
                  % curve,
                  "HTTP 200, content %d B" % len(PAGE) in got)
        finally:
            ec_stop()

    # --- A two-level chain ---
    # Only the root is trusted, so the system has to go through
    # the intermediate the server sends along
    ch_port, ch_stop = start_https_chain_server()
    if ch_port is None:
        print("SKIPPED (chain): no openssl")
    else:
        try:
            run(q, "run httpget 10.0.2.2 %d /rootca.der roots.der"
                % http_port, 60)
            got = run(q, "run httpget https://threados.test %d /"
                      % ch_port, 120)
            said = [ln.strip() for ln in got.splitlines()
                    if "[httpget]" in ln][-2:]
            check("a chain through an intermediate closed up to a "
                  "trusted root (%s)" % said,
                  "certificate checked" in got and
                  "HTTP 200, content %d B" % len(PAGE) in got)
        finally:
            ch_stop()

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
            got = run(q, "run httpget -k https://threados.test "
                      "%d /index.html" % sport, 90)
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
            run(q, "run httpget 10.0.2.2 %d /tls12.der roots.der"
                % http_port, 60)
            got = run(q, "run httpget https://threados.test "
                      "%d /index.html" % sport, 120)
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
            got = run(q, "run httpget https://threados.test "
                      "%d /index.html" % sport, 120)
            check("a TLS 1.2 server with a foreign certificate is "
                  "refused", "no trusted issuer" in got and
                  "HTTP 200" not in got)
    finally:
        sstop()
        shutil.rmtree(sdir, ignore_errors=True)


def browser(iso, disk, http_port):
    """The browser in a desktop window: network -> GUI. It ties
    everything together at once: the keyboard and mouse from the
    compositor, DNS, TCP, the HTML parser and drawing in a window - and
    all of it from ring 3. The page comes over plain HTTP, so it needs
    the name server and not OpenSSL."""
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


if __name__ == "__main__":
    sys.exit(main())
