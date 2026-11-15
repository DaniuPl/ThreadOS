#!/usr/bin/env python3
"""ThreadOS smoke test: the simple graphical browser (gweb) - the network,
a window and the disk in one road.

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
    # A TCP server in ring 3 used to open this group; on the 64-bit
    # kernel that is t_socket's, where the host connects to a server
    # in the guest through the C library's sockets.

    # --- The browser in a desktop window (network -> GUI -> disk) ---
    print("--- the desktop: the browser (gweb) ---")
    wdisk = tempfile.mktemp(suffix=".img")
    with open(wdisk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    http_port, http_stop = start_http_server()
    dns_stop, dns_ok = start_dns_server()
    if not dns_ok:
        # The browser looks the page up by NAME, and what comes after it
        # reads back what the browser saved - so with no name server of
        # ours there is nothing here left to check.
        print("SKIPPED (the browser): " + DNS_HINT)
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

        ink = theme("dark")["app_text"]

        def page_white():
            """How much text is on the page - under the toolbar and the
            status line: pixels in the theme's text colour, or nearly"""
            _, _, a = q_web.screen_raw()
            return sum(1 for y in range(140, 390) for x in range(96, 520)
                       if near(a(x, y), ink, 40))

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
        for ch in "threados.test:%d/" % http_port:   # an address by name
            q_web.sendkey(Qemu.KEYNAMES.get(ch, ch))
        q_web.sendkey("ret")

        shown = False
        for _ in range(25):
            if page_white() > before + 50:
                shown = True
                break
            time.sleep(0.4)
        check("the browser resolved the name, fetched the page and "
              "drew the text", shown)

        # --- scrolling and links ---
        for _ in range(40):              # a new address: a page with a link
            q_web.sendkey("backspace")
        for ch in "threados.test:%d/links" % http_port:
            q_web.sendkey(Qemu.KEYNAMES.get(ch, ch))
        q_web.sendkey("ret")
        time.sleep(5)

        link_ink = theme("dark")["accent"]

        def link_pixels():
            """Links are drawn in the accent - which is what a link has
            been since there were links - and underlined"""
            _, _, a = q_web.screen_raw()
            return [(x, y) for y in range(140, 390) for x in range(96, 520)
                    if near(a(x, y), link_ink, 20)]

        first_view = page_white()
        q_web.sendkey("pgdn")
        time.sleep(1.0)
        check("PgDn scrolls the content in the window", page_white() != first_view)

        found = []
        for _ in range(8):               # scroll to the end, where the link is
            found = link_pixels()
            if found:
                break
            q_web.sendkey("pgdn")
            time.sleep(0.8)
        check("the link stands out in colour in the text", bool(found))

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
    # restart. The browser ran in the signed-in profile and saved the
    # page in its home, which the text shell's guest cannot read - so
    # it signs in as that profile first.
    slog2 = tempfile.mktemp(suffix=".log")
    q_web2 = Qemu(KERNEL, INITRD, wdisk, append=TEXT_MODE, serial=slog2)
    try:
        q_web2.wait_serial("thread64>", timeout=60)
        q_web2.command("login admin", "password:", timeout=20)
        q_web2.command("thread", "logged in as admin", timeout=20)
        saved = q_web2.command("cat page.htm", "The second page", timeout=20)
        check("the clicked link fetched the second page (it shows on the disk)",
              "The link worked" in saved)
        check("the page the browser saved survived the restart",
              "<html><body><h1>The second page</h1>" in saved)
    finally:
        q_web2.close()
        for f in (wdisk, slog2):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
