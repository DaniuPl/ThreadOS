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

  WebKit is asked for it as a phone's browser asks, naming ThreadOS
  1.0: "Linux; Android 10; ThreadOS 1.0", "Mobile Safari", "Weft/1.0" -
  what sites send their newest pages for phones by

  the page's script measured it: the boxes' places and sizes and a
  number written the Polish way by Intl (ICU) are what Chromium
  (Playwright, on this machine) and Node's Intl make of the same page
  at the same size - the phone's window below Weft's band, in CSS
  pixels: 360 wide at the phone's 200%

  the window on the phone's screen shows it at that scale, sharp: the
  red box 400 x 200 of the panel's pixels of its exact colour, the blue
  one with its corners rounded off (cut off at the screen's edge, the
  page being wider than a phone), the flex items green, brown and
  green in the widths measured, twice over; and around it Weft as a
  phone's browser, drawn by the phone (WIN_DRAW) at the panel's
  resolution: the page's address alone at the top, without its scheme,
  and the toolbar at the bottom, its tabs button counting one

and a second page, for the finger: it is 360 CSS pixels wide at a
devicePixelRatio of 2; a tap on its box is a click there, and a drag
up scrolls it - by as much as its script reads in scrollY, and the
screen shows the band far down the page exactly that much higher,
twice as many of the panel's pixels; the toolbar's + opens a second
tab, and its tabs button shows both as cards side by side - the count
in the heading, each card's title, and on the first the picture of its
page as it was left, the red band in it; a tap on that card brings it
back, the band where it was, and a card's x closes its tab
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
K = 2                    # the phone's 200%: the panel's pixels to a CSS one
PANEL = (720, 1280)
WEFT_SRC = "apps/webkit/weft.c"
UD_PX = define("user/udraw.h", "UD_PX")


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
    bar = define(MOB, "MOB_BAR_H")
    top, bottom = define(WEFT_SRC, "M_TOP_H"), define(WEFT_SRC, "M_BOT_H")
    safe = define(MOB, "MOB_SAFE_UP")
    width = PANEL[0] // K
    height = PANEL[1] // K - bar - top - bottom - safe
    want = chromium_title(width, height)
    if want is None:
        print("NOTE: no Chromium through Playwright on this machine - "
              "the layout is held to the numbers worked out by hand")
        want = ("ready red=20,20,200,100 blue=260,20,120,120 "
                "row=20+95 125+190 325+95 num=" + (node_number() or "?"))
    num = node_number()

    agents = []

    def page(h, body):
        agents.append(h.headers.get("User-Agent") or "")
        return 200, [("Content-Type", "text/html; charset=utf-8")], PAGE
    ROUTES["/page.html"] = page
    ROUTES["/tall.html"] = lambda h, body: (
        200, [("Content-Type", "text/html; charset=utf-8")], TALL)
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
        ua = agents[0] if agents else ""
        check(f"as a phone's browser, ThreadOS 1.0's: {ua!r}",
              re.match(r"Mozilla/5\.0 \(Linux; Android 10; ThreadOS 1\.0\) "
                       r"AppleWebKit/[\d.]+ \(KHTML, like Gecko\) "
                       r"Version/[\d.]+ Mobile Safari/[\d.]+ Weft/1\.0$", ua)
              is not None)
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
        check(f"the red box is on the phone's screen, 200 x 100 CSS "
              f"pixels, 400 x 200 of the panel's ({red})",
              red is not None and red[2] - red[0] == 200 * K and
              red[3] - red[1] == 100 * K)
        if red is None:
            return summary()
        ox, oy = red[0] - 20 * K, red[1] - 20 * K
        check(f"below the status bar and Weft's address (the page starts "
              f"at {ox},{oy})", (ox, oy) == (0, (bar + top) * K))
        blue = find_box(at, sw, sh, BLUE,
                        (ox + 250 * K, oy + 10 * K, PANEL[0], oy + 150 * K))
        check(f"the blue box beside it, cut off by the screen's edge "
              f"({blue})",
              blue == (ox + 260 * K, oy + 20 * K, PANEL[0], oy + 140 * K))
        check("with its corners rounded off",
              at(ox + 260 * K, oy + 20 * K) != BLUE and
              at(ox + 320 * K, oy + 20 * K) == BLUE)
        row = [at(ox + x * K, oy + 180 * K) for x in (20, 114, 125, 314, 325)]
        check(f"the flex items green, brown and green, 95 and 190 CSS "
              f"pixels wide ({row})",
              row == [GREEN, GREEN, BROWN, BROWN, GREEN])
        # Weft around it, drawn by the phone at its scale: the address,
        # without its scheme, and the toolbar's tabs button, counting
        px = define(WEFT_SRC, "M_TEXT_PX")
        base = bar + baseline(define(WEFT_SRC, "M_PILL_Y"),
                              define(WEFT_SRC, "M_PILL_H"), px)
        url = f"10.0.2.2:{port}/page.html"
        shown = q.find_ui((define(WEFT_SRC, "M_PILL_X") + 8) * K,
                          (width - 44) * K, base * K, px=px * K,
                          charset="hmtp:/.0123456789agel",
                          screen=(sw, sh, at))
        check(f"Weft's address at the top, without its scheme ({shown!r})",
              shown == url)
        bx = button_x(3, width)
        cy = PANEL[1] // K - safe - bottom // 2
        count = q.find_ui((bx - 8) * K, (bx + 8) * K,
                          baseline(cy - 11, 22, 12) * K, px=12 * K,
                          face="bold", charset="0123456789",
                          screen=(sw, sh, at))
        check(f"and its toolbar at the bottom, the tabs button counting one "
              f"({count!r})", count == "1")
        typing(q, port)
    finally:
        q.close()
        stop()
    fingers()
    return summary()


PHONE_H = Phone.LOGICAL[1]


def baseline(top, h, px):
    """udraw.h's ud_baseline: text set in the middle of a box"""
    return top + (h + (px * 73 + 50) // 100) // 2


def button_x(i, width):
    """weft.c's button_x: the toolbar's buttons evenly across"""
    n = define(WEFT_SRC, "M_BUTTONS")
    return width * (2 * i + 1) // (2 * n)


def card_box(i, width):
    """weft.c's card_box: cards in as many columns as fit, two at
    least, under the heading"""
    gap = define(WEFT_SRC, "M_GAP")
    n = max(2, (width - gap) // (define(WEFT_SRC, "M_CARD_W") + gap))
    cw = (width - (n + 1) * gap) // n
    return (gap + (i % n) * (cw + gap),
            define(WEFT_SRC, "M_HEAD_H") + (i // n) *
            (define(WEFT_SRC, "M_CARD_H") + gap), cw)


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
        size = wait_line(q, seen, "[weft] title tall ", 60)
        check(f"360 CSS pixels wide, at a devicePixelRatio of 2 ({size})",
              size == f"[weft] title tall {PANEL[0] // K} {K}")
        # where the page starts on the screen: its blue box at 20,20
        blue = None
        end = time.time() + 60
        while blue is None and time.time() < end:
            sw, sh, at = q.screen_raw()
            blue = find_box(at, sw, sh, BLUE)
            if blue is None:
                time.sleep(1)
        check(f"its box is on the screen, 400 x 200 ({blue})",
              blue is not None and blue[2] - blue[0] == 200 * K and
              blue[3] - blue[1] == 100 * K)
        if blue is None:
            return
        ox, oy = blue[0] - 20 * K, blue[1] - 20 * K
        check(f"the band, {BAND_TOP} CSS pixels down the page, is not yet",
              find_box(at, sw, sh, RED) is None)

        seen = len(q.serial())
        q.tap((blue[0] + 100) / K, (blue[1] + 50) / K)
        check("a tap on the box is a click on it",
              wait_line(q, seen, "[weft] title tapped", 60) is not None)

        # a drag of 450 logical pixels up - above the bottom edge, where
        # a drag up is the phone's, the home gesture
        edge = PHONE_H - define(MOB, "MOB_EDGE")
        seen = len(q.serial())
        q.touch([(180, edge - 24 - 30 * i) for i in range(16)])
        line = wait_line(q, seen, "[weft] title scrolled ", 120)
        moved = int(line.split()[-1]) if line else 0
        check(f"a drag up scrolls the page ({line})", moved >= 450)
        want = (ox, oy + (BAND_TOP - moved) * K, ox + PANEL[0],
                min(oy + (BAND_TOP + BAND_H - moved) * K, PANEL[1]))
        band = None
        end = time.time() + 60
        while time.time() < end:
            sw, sh, at = q.screen_raw()
            band = find_box(at, sw, sh, RED)
            if band == want:
                break
            time.sleep(1)
        check(f"and the screen shows it scrolled {moved} CSS pixels: the "
              f"band at {band}, where the page puts it {want}", band == want)
        tabs(q, band)
    finally:
        q.close()
        stop()


def typing(q, port):
    """A tap on the address brings the phone's keyboard up; what is typed
    on it goes there, and Enter goes to it"""
    from t_arm64apps import KB_TOP, LETTERS, PITCH, note_key, a
    seen = len(q.serial())
    q.tap(Phone.LOGICAL[0] // 2,
          define(MOB, "MOB_BAR_H") + define(WEFT_SRC, "M_PILL_Y") +
          define(WEFT_SRC, "M_PILL_H") // 2)
    up = wait_line(q, seen, "[winmob] window ", 30)
    check(f"a tap on the address brings the keyboard up ({up})",
          up is not None and up.endswith("keyboard up"))
    step, h = a("NOTES_KEY_STEP"), a("NOTES_KEY_H")
    bottom_row = KB_TOP + 3 * step + h // 2
    layer = (31, bottom_row)                       # "123" / "ABC"
    dot = (Phone.LOGICAL[0] - 82, bottom_row)
    enter = (Phone.LOGICAL[0] - 33, bottom_row)
    digit_rows = ["1234567890", "-/:;()&@\"'", ".,?!#%+"]
    letters = True
    for c in f"10.0.2.2:{port}/tall.html":
        if c == ".":
            q.touch([dot], settle=0.3)
            continue
        want = c.isalpha()
        if want != letters:
            q.touch([layer], settle=0.3)
            letters = want
        rows = LETTERS if letters else digit_rows
        r = next(i for i, row in enumerate(rows) if c in row)
        q.touch([note_key(r, rows[r].index(c), len(rows[r]))], settle=0.3)
    seen = len(q.serial())
    q.tap(*enter)
    go = wait_line(q, seen, "[weft] go ", 30)
    check(f"typed on it, the address goes there with Enter ({go})",
          go == f"[weft] go http://10.0.2.2:{port}/tall.html")
    done = wait_line(q, seen, f"[weft] load finished http://10.0.2.2:{port}"
                     f"/tall.html", 120)
    down = wait_line(q, seen, "[winmob] window ", 30)
    check(f"and loads, the keyboard down again ({down})",
          done is not None and down is not None and
          down.endswith("keyboard down"))


def tabs(q, band):
    """The toolbar's + and tabs buttons, and the cards"""
    bar = define(MOB, "MOB_BAR_H")
    width = PANEL[0] // K
    cy = PHONE_H - define(MOB, "MOB_SAFE_UP") - define(WEFT_SRC, "M_BOT_H") // 2
    seen = len(q.serial())
    q.tap(button_x(4, width), cy)
    check("the toolbar's + opens a second tab",
          wait_line(q, seen, "[weft] tab 2 of 2", 60) is not None)
    wait_line(q, seen, "[weft] title New tab", 60)
    seen = len(q.serial())
    q.tap(button_x(3, width), cy)
    shown = wait_line(q, seen, "[weft] tabs shown", 30)
    time.sleep(2)
    sw, sh, at = q.screen_raw()
    head = q.find_ui(0, width * K // 2, (bar + baseline(
        0, define(WEFT_SRC, "M_HEAD_H"), 17)) * K, px=17 * K, face="bold",
        charset="0123456789 tabs", screen=(sw, sh, at))
    check(f"and its tabs button shows them side by side: {head!r}",
          shown is not None and head == "2 tabs")
    bar_h = define(WEFT_SRC, "M_CARD_BAR")
    titles, reds = [], []
    for i in range(2):
        x, y, cw = card_box(i, width)
        titles.append(q.find_ui((x + 6) * K, (x + cw - 36) * K,
                                (bar + baseline(y, bar_h, UD_PX)) * K,
                                px=UD_PX * K, face="bold",
                                screen=(sw, sh, at)))
        reds.append(sum(1 for yy in range((bar + y + bar_h) * K,
                                          (bar + y + define(WEFT_SRC,
                                                            "M_CARD_H")) * K, 2)
                        for xx in range(x * K, (x + cw) * K, 2)
                        if at(xx, yy) == RED))
    check(f"each card titled by its page ({titles})",
          titles[0].startswith("scrolled ") and titles[1] == "New tab")
    check(f"the first with its page's picture, the red band in it, the new "
          f"one's without ({reds} red pixels)", reds[0] > 100 and reds[1] == 0)
    x, y, cw = card_box(0, width)
    seen = len(q.serial())
    q.tap(x + cw // 2, bar + y + 100)
    back = wait_line(q, seen, "[weft] tab 1 of 2", 30)
    got = None
    end = time.time() + 30
    while time.time() < end:
        sw, sh, at = q.screen_raw()
        got = find_box(at, sw, sh, RED)
        if got == band:
            break
        time.sleep(1)
    check(f"a tap on the first card brings its page back, the band where "
          f"it was ({got})", back is not None and got == band)
    seen = len(q.serial())
    q.tap(button_x(3, width), cy)
    wait_line(q, seen, "[weft] tabs shown", 30)
    x, y, cw = card_box(1, width)
    seen = len(q.serial())
    q.tap(x + cw - 17, bar + y + bar_h // 2)
    check("and a card's x closes its tab",
          wait_line(q, seen, "[weft] tab 1 of 1", 30) is not None)


if __name__ == "__main__":
    sys.exit(main())
