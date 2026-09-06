#!/usr/bin/env python3
"""ProtonOS smoke test: a TCP server in ring 3 and the graphical browser.

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
    # --- ProtonOS as a TCP server (a passive open, a separate boot) ---
    print("--- siec: serwer TCP w ring 3 ---")
    srv_port = 5555
    q_srv = Qemu(KERNEL, INITRD, hostfwd_port=srv_port, append=TEXT_MODE)
    try:
        q_srv.wait_for("ProtonOS shell")
        q_srv.wait_for("[siec] DHCP", timeout=20)
        q_srv.type_line("run tcpserv")
        view = q_srv.wait_for("nasluchuje na porcie 5555", timeout=15)
        check("ring 3 nasluchuje na porcie TCP",
              "nasluchuje na porcie 5555" in view)

        # The host's real stack connects (through QEMU's hostfwd)
        sock = socket.create_connection(("127.0.0.1", srv_port), timeout=15)
        sock.settimeout(15)
        greeting = sock.recv(200)
        check("host polaczyl sie i dostal powitanie od ProtonOS-a",
              b"ProtonOS TCP gotowy" in greeting)

        sock.sendall(b"czesc z hosta\n")
        echoed = sock.recv(200)
        check("dane z hosta wrocily odbite przez serwer w ring 3",
              b"czesc z hosta" in echoed)
        sock.close()

        seen = q_srv.wait_for("od klienta", timeout=15)
        check("serwer zobaczyl dane klienta",
              "od klienta: czesc z hosta" in seen)
    finally:
        q_srv.close()

    # --- The browser in a desktop window (network -> GUI -> disk) ---
    print("--- pulpit: przegladarka (gweb) ---")
    wdisk = tempfile.mktemp(suffix=".img")
    with open(wdisk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    http_port, http_stop = start_http_server()
    dns_stop, dns_ok = start_dns_server()
    if not dns_ok:
        # The browser looks the page up by NAME, and what comes after it
        # reads back what the browser saved - so with no name server of
        # ours there is nothing here left to check.
        print("POMINIETE (przegladarka): " + DNS_HINT)
        http_stop()
        os.unlink(wdisk)
        return summary()
    q_web = Qemu(KERNEL, INITRD, wdisk)
    try:
        q_web.wait_desktop()
        time.sleep(2.0)                  # a moment for the DHCP lease

        # The host's DNS server: the browser will look the address up by
        # name. The command goes to the kernel's shell - in graphics mode
        # it listens to the keyboard as long as no window has focus.
        q_web.type_line("dns 10.0.2.2")
        time.sleep(0.5)
        time.sleep(1.0)
        q_web.type_line("run gweb")
        time.sleep(1.5)

        def page_white():
            """How much text is on the page. The page is the window's
            light grey, so the text on it is BLACK - white on that grey
            was drawn and could not be read."""
            _, _, a = q_web.screen_raw()
            return sum(1 for y in range(100, 250) for x in range(96, 400)
                       if a(x, y) == (0, 0, 0))

        before = page_white()

        # Calibrating the cursor - clicks on links have to land to the pixel,
        # and mouse deltas count from the current position
        q_web.mouse_home()

        # A click in the window gives it keyboard focus; then the cursor goes
# aside
        q_web.mouse_goto(150, 120)
        q_web.mouse_click()
        q_web.mouse_goto(400, 350)
        time.sleep(0.3)

        for _ in range(20):              # clear the default address
            q_web.sendkey("backspace")
        for ch in "protonos.test:%d/" % http_port:   # an address by name
            q_web.sendkey(Qemu.KEYNAMES.get(ch, ch))
        q_web.sendkey("ret")

        shown = False
        for _ in range(25):
            if page_white() > before + 50:
                shown = True
                break
            time.sleep(0.4)
        check("przegladarka rozwiazala nazwe, pobrala strone i "
              "narysowala tekst", shown)

        # --- scrolling and links ---
        for _ in range(40):              # a new address: a page with a link
            q_web.sendkey("backspace")
        for ch in "protonos.test:%d/linki" % http_port:
            q_web.sendkey(Qemu.KEYNAMES.get(ch, ch))
        q_web.sendkey("ret")
        time.sleep(5)

        def link_pixels():
            """Links are drawn in the accent blue - which is what a link
            has been since there were links, and which is readable on a
            light page in a way yellow is not."""
            _, _, a = q_web.screen_raw()
            return [(x, y) for y in range(180, 260) for x in range(96, 420)
                    if a(x, y) == (0, 0, 255)]

        first_view = page_white()
        q_web.sendkey("pgdn")
        time.sleep(1.0)
        check("PgDn przewija tresc w oknie", page_white() != first_view)

        found = []
        for _ in range(8):               # scroll to the end, where the link is
            found = link_pixels()
            if found:
                break
            q_web.sendkey("pgdn")
            time.sleep(0.8)
        check("odnosnik wyrozniony kolorem w tekscie", bool(found))

        if found:
            # A link can occupy two lines - the centre of gravity of all the
            # pixels would then fall in the gap between them. We take the
            # densest line and its centre.
            rows = {}
            for x, y in found:
                rows.setdefault(y, []).append(x)
            best_y = max(rows, key=lambda y: len(rows[y]))
            xs = sorted(rows[best_y])
            cx, cy = xs[len(xs) // 2], best_y
            q_web.mouse_goto(cx, cy)
            q_web.mouse_click()
            q_web.mouse_goto(560, 400)
            time.sleep(5)
    finally:
        q_web.close()
        http_stop()
        dns_stop()

    # The same disk in text mode: content from the world survived the
# restart
    q_web2 = Qemu(KERNEL, INITRD, wdisk, append=TEXT_MODE)
    try:
        q_web2.wait_for("ProtonOS shell")
        q_web2.type_line("cat strona.htm")
        saved = q_web2.wait_for("Strona druga", timeout=10)
        check("klikniety odnosnik pobral druga strone (widac ja na dysku)",
              "Odnosnik zadzialal" in saved)
        check("strona zapisana przez przegladarke przezyla restart",
              "<html><body><h1>Strona druga</h1>" in saved)
    finally:
        q_web2.close()
        if os.path.exists(wdisk):
            os.unlink(wdisk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
