#!/usr/bin/env python3
"""ThreadOS smoke test: Weft on the phone.

WPE WebKit's three processes built for aarch64 (`make weft-arm64`, the
PC's rules with TARGET=aarch64) and handed to the ARM64 kernel as the
package weft.tapp through fw_cfg, on QEMU's virt machine as a phone -
ramfb, a touchscreen, a virtio network card and entropy device. weft
opens a window on the phone (kernelarm64/winmob.c) through the same
platform as on the PC (apps/webkit/wpethreados.c); WebKit starts
wpenetproc, which fetches the page with libsoup over the PC's TCP/IP
stack, and wpewebproc, which parses it, runs its script in
JavaScriptCore, lays it out and paints it with Skia into shared memory
that weft copies into its window. t_webkit.py's page, from the host:

  the kernel takes the package, and weft - started on the command
  line - fetches and loads the page

  the page's script measured it: the boxes' places and sizes and a
  number written the Polish way by Intl (ICU) are what Chromium
  (Playwright, on this machine) and Node's Intl make of the same page
  at the same size - the phone's window below Weft's band

  the window on the phone's screen shows it: the red box 200 x 100 of
  its exact colour, the blue one 120 x 120 with its corners rounded off,
  the flex items green, brown and green in the widths measured

and a second page, for the finger: a tap on its box is a click there,
and a drag up scrolls it - by as much as its script reads in scrollY,
and the screen shows the band far down the page exactly that much
higher
"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, noarm64, Phone, ARM64_INITRD, ROUTES,
                   start_http_server, _kernel_define as define)
from t_webkit import (PAGE, TALL, BAND_TOP, BAND_H, chromium_title,
                      node_number, find_box, RED, BLUE, GREEN, BROWN)

WEFT = os.path.join(os.path.dirname(ARM64_INITRD), "aarch64", "weft.tapp")
MOB = "kernelarm64/mobile.c"
K = 2
PANEL = (720, 1280)


def wait_line(q, seen, prefix, timeout):
    """The first line from 'seen' on that starts so - read as UTF-8, as
    the programs write it (q.serial() hands back bytes as Latin-1)"""
    end = time.time() + timeout
    while time.time() < end:
        out = q.serial()[seen:].encode("latin1").decode("utf-8", "replace")
        for l in out.replace("\r", "").split("\n"):
            if l.startswith(prefix):
                return l
        time.sleep(0.5)
    return None


def main():
    print("--- Weft on the phone ---")
    why = noarm64()
    if why:
        print("SKIPPED: " + why)
        return summary()
    if not os.path.exists(WEFT):
        print("SKIPPED: no aarch64 weft.tapp (make weft-arm64)")
        return summary()
    # The page's viewport: the window - the screen below the status bar -
    # less Weft's band
    bar = define(MOB, "MOB_BAR_H") * K
    width = PANEL[0]
    height = PANEL[1] - bar - define("apps/webkit/weft.c", "CHROME_H")
    want = chromium_title(width, height)
    if want is None:
        print("NOTE: no Chromium through Playwright on this machine - "
              "the layout is held to the numbers worked out by hand")
        want = ("ready red=20,20,200,100 blue=260,20,120,120 "
                "row=20+95 125+190 325+95 num=" + (node_number() or "?"))
    num = node_number()

    ROUTES["/page.html"] = lambda h, body: (
        200, [("Content-Type", "text/html; charset=utf-8")], PAGE)
    port, stop = start_http_server()
    q = Phone(cores=4, mib=2048,
              append=f"run=weft,http://10.0.2.2:{port}/page.html",
              extra=["-fw_cfg", "name=opt/threados/apps/weft,file=" + WEFT,
                     "-netdev", "user,id=n0",
                     "-device", "virtio-net-device,netdev=n0"])
    try:
        out = q.wait_serial("[mobile] lock screen", timeout=180)
        check("the kernel takes Weft's package",
              re.search(r"Package weft \d+: Weft, ", out) is not None)
        q.touch([(180, 560), (180, 420), (180, 300)])
        seen = 0
        done = wait_line(q, seen, "[weft] load finished", 600)
        check(f"WebKit fetches and loads the page ({done})",
              done is not None and "/page.html" in done)
        title = wait_line(q, seen, "[weft] title ready", 300)
        got = title[len("[weft] title "):] if title else None
        check(f"the page's script ran and measured the page: ThreadOS "
              f"{got!r}, Chromium {want!r}", got == want)
        check(f"Intl wrote the number the Polish way, as Node's ICU does "
              f"({num!r})", got is not None and num is not None and
              got.endswith("num=" + num))
        frame = wait_line(q, seen, "[weft] frame shown", 300)
        check("a frame of the page reaches the window", frame is not None)

        red = None
        end = time.time() + 120
        while red is None and time.time() < end:
            sw, sh, at = q.screen_raw()
            red = find_box(at, sw, sh, RED)
            if red is None:
                time.sleep(1)
        check(f"the red box is on the phone's screen, 200 x 100 ({red})",
              red is not None and red[2] - red[0] == 200 and
              red[3] - red[1] == 100)
        if red is None:
            return summary()
        ox, oy = red[0] - 20, red[1] - 20
        check(f"below the status bar and Weft's band (the page starts at "
              f"{ox},{oy})", ox == 0 and oy >= bar)
        blue = find_box(at, sw, sh, BLUE,
                        (ox + 250, oy + 10, ox + 390, oy + 150))
        check(f"the blue box beside it, 120 x 120 ({blue})",
              blue == (ox + 260, oy + 20, ox + 380, oy + 140))
        check("with its corners rounded off",
              at(ox + 260, oy + 20) != BLUE and at(ox + 320, oy + 20) == BLUE)
        row = [at(ox + x, oy + 180) for x in (20, 114, 125, 314, 325, 419)]
        check(f"the flex items green, brown and green, 95, 190 and 95 "
              f"pixels wide ({row})",
              row == [GREEN, GREEN, BROWN, BROWN, GREEN, GREEN])
    finally:
        q.close()
        stop()
    fingers()
    return summary()


PHONE_H = Phone.LOGICAL[1]


def fingers():
    """A tap is a click where the finger went down; a drag scrolls the
    page with the finger, and the screen shows the page where its
    script says it is"""
    print("--- Weft and the finger ---")
    ROUTES["/tall.html"] = lambda h, body: (
        200, [("Content-Type", "text/html; charset=utf-8")], TALL)
    port, stop = start_http_server()
    q = Phone(cores=4, mib=2048,
              append=f"run=weft,http://10.0.2.2:{port}/tall.html",
              extra=["-fw_cfg", "name=opt/threados/apps/weft,file=" + WEFT,
                     "-netdev", "user,id=n0",
                     "-device", "virtio-net-device,netdev=n0"])
    try:
        q.wait_serial("[mobile] lock screen", timeout=180)
        q.touch([(180, 560), (180, 420), (180, 300)])
        seen = 0
        shown = wait_line(q, seen, "[weft] frame shown", 600)
        check("the tall page is shown", shown is not None)
        if shown is None:
            return
        # where the page starts on the screen: its blue box at 20,20
        blue = None
        end = time.time() + 60
        while blue is None and time.time() < end:
            sw, sh, at = q.screen_raw()
            blue = find_box(at, sw, sh, BLUE)
            if blue is None:
                time.sleep(1)
        check(f"its box is on the screen, 200 x 100 ({blue})",
              blue is not None and blue[2] - blue[0] == 200 and
              blue[3] - blue[1] == 100)
        if blue is None:
            return
        ox, oy = blue[0] - 20, blue[1] - 20
        check("the band, 1600 pixels down the page, is not yet",
              find_box(at, sw, sh, RED) is None)

        seen = len(q.serial())
        q.tap((blue[0] + 100) / K, (blue[1] + 50) / K)
        check("a tap on the box is a click on it",
              wait_line(q, seen, "[weft] title tapped", 60) is not None)

        # a drag of 450 logical pixels up, 900 of the panel's - above the
        # bottom edge, where a drag up is the phone's, the home gesture
        edge = PHONE_H - define(MOB, "MOB_EDGE")
        seen = len(q.serial())
        q.touch([(180, edge - 24 - 30 * i) for i in range(16)])
        line = wait_line(q, seen, "[weft] title scrolled ", 120)
        moved = int(line.split()[-1]) if line else 0
        check(f"a drag up scrolls the page ({line})", moved >= 450)
        want = (ox, oy + BAND_TOP - moved, ox + PANEL[0],
                min(oy + BAND_TOP + BAND_H - moved, PANEL[1]))
        band = None
        end = time.time() + 60
        while time.time() < end:
            sw, sh, at = q.screen_raw()
            band = find_box(at, sw, sh, RED)
            if band == want:
                break
            time.sleep(1)
        check(f"and the screen shows it scrolled {moved} pixels: the band "
              f"at {band}, where the page puts it {want}", band == want)
    finally:
        q.close()
        stop()


if __name__ == "__main__":
    sys.exit(main())
