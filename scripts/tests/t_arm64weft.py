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

and a second page, for the finger, with the phone in Liquid Glass -
the toolbar the page under it, frosted, lifted and tinted: it is 360
CSS pixels wide at a
devicePixelRatio of 2; a tap on its box is a click there, and a drag
up scrolls it - by as much as its script reads in scrollY, and the
screen shows the band far down the page exactly that much higher,
twice as many of the panel's pixels; the toolbar's menu opens a second
tab, and its tabs button shows both as cards side by side - the count
in the heading, each card's title, and on the first the picture of its
page as it was left, the red band in it; a tap on that card brings it
back, the band where it was, and a card's x closes its tab; the menu's
History lists the pages visited, newest first, and a tap goes back to
one; its Settings ask for the desktop site - the next request is a
computer's, ThreadOS 1.0 on aarch64 - and clear the history
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
from t_webkit import (PAGE, TALL, TALL_BG, TICKED, BAND_TOP, BAND_H,
                      chromium_title,
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
    safe = define(MOB, "MOB_SAFE_UP")
    width = PANEL[0] // K
    height = PANEL[1] // K - bar - safe       # the page: the window whole
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
        check(f"the page fills the window, from just below the status bar "
              f"(it starts at {ox},{oy})", (ox, oy) == (0, bar * K))
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
        # Weft over it, drawn by the phone at its scale: the floating
        # toolbar - the site without its scheme, the tabs counted - in
        # the phone's look, matte here: its tint over the white page
        bx, by, bw, bh = bar_box(width)
        px = define(WEFT_SRC, "M_TEXT_PX")
        base = bar + baseline(by, bh, px)
        url = f"10.0.2.2:{port}"
        shown = q.find_ui((bar_button(1, width) + 22) * K,
                          (bar_button(2, width) - 22) * K, base * K,
                          px=px * K, face="bold",
                          charset="hmtp:/.0123456789agel",
                          screen=(sw, sh, at))
        check(f"the floating toolbar has the site alone, as a phone's "
              f"browser shows it ({shown!r})", shown == url)
        tx = bar_button(2, width)
        count = q.find_ui((tx - 8) * K, (tx + 8) * K,
                          (bar + baseline(by + bh // 2 - 11, 22, 12)) * K,
                          px=12 * K, face="bold", charset="0123456789",
                          screen=(sw, sh, at))
        check(f"and the tabs, counted ({count!r})", count == "1")
        a = define(MOB, "MOB_MATTE_A")
        tint = rgb(hexdef(MOB, "MOB_MATTE_DARK"))
        want = tuple((c * a + 255 * (255 - a) + 127) // 255 for c in tint)
        got = at((bx + 46) * K, (bar + by + 12) * K)
        check(f"matte, as the phone's Settings have it: its tint over the "
              f"page, {got} ({want} worked out)", got == want)
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


def rgb(v):
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def hexdef(path, name):
    """A colour's #define (0xRRGGBB), which the decimal reader misses"""
    src = open(os.path.join(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))), path)).read()
    return int(re.search(r"#define\s+%s\s+0x([0-9A-Fa-f]+)" % name,
                         src).group(1), 16)


WIN_H = PHONE_H_ = Phone.LOGICAL[1] - define(MOB, "MOB_BAR_H") - \
    define(MOB, "MOB_SAFE_UP")


def bar_box(width, win_h=None):
    """weft.c's bar_box: the floating toolbar, in the window"""
    win_h = win_h or WIN_H
    bw = min(width - 2 * define(WEFT_SRC, "G_BAR_SIDE"),
             define(WEFT_SRC, "G_BAR_MAXW"))
    bh = define(WEFT_SRC, "G_BAR_H")
    return (width - bw) // 2, win_h - define(WEFT_SRC, "G_BAR_GAP") - bh, bw, bh


def bar_button(i, width):
    """weft.c's bar_button: back, forward, the tabs, the menu"""
    x, y, w, h = bar_box(width)
    return [x + 28, x + 64, x + w - 74, x + w - 30][i]


def bar_tap(q, i, width):
    """A tap on one of the toolbar's buttons - or, i None, its address"""
    x, y, w, h = bar_box(width)
    q.tap(x + w // 2 if i is None else bar_button(i, width),
          define(MOB, "MOB_BAR_H") + y + h // 2)


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
    agents = []

    def tall(h, body):
        agents.append(h.headers.get("User-Agent") or "")
        return 200, [("Content-Type", "text/html; charset=utf-8")], TALL
    ROUTES["/tall.html"] = tall
    port, stop = start_http_server()
    q = Phone(cores=4, mib=2048,
              append=f"theme=glass run=weft,http://10.0.2.2:{port}/tall.html",
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
        # the toolbar in Liquid Glass, as the phone was started with:
        # the page under it frosted (one colour here, so as it was),
        # brighter, and tinted towards the dark look's surface
        bx, by, bw, bh = bar_box(PANEL[0] // K)
        lift = define(MOB, "GLASS_LIFT")
        tint_a = define(MOB, "FROST_TINT")
        surface = (0x1C, 0x1C, 0x1E)
        want = tuple(((v + ((255 - v) * lift >> 8)) * (256 - tint_a) +
                      s * tint_a) >> 8 for v, s in zip(TALL_BG, surface))
        spot = ((bx + 46) * K, (define(MOB, "MOB_BAR_H") + by + bh // 2) * K)
        got = at(*spot)
        check(f"the toolbar in Liquid Glass: the page under it frosted, "
              f"lifted and tinted, {got} ({want} worked out)", got == want)

        seen = len(q.serial())
        q.tap((blue[0] + 100) / K, (blue[1] + 50) / K)
        check("a tap on the box is a click on it",
              wait_line(q, seen, "[weft] title tapped", 60) is not None)
        # ... which repaints a small part of the page ten times: frames
        # that bring only that part. Under the toolbar the page is
        # still the page, not the toolbar's glass from the frame before
        # laid under the glass again, darker and more blurred each time
        ticked = None
        end = time.time() + 30
        while ticked is None and time.time() < end:
            time.sleep(0.5)
            sw, sh, at = q.screen_raw()
            ticked = find_box(at, sw, sh, TICKED)
        got = at(*spot)
        check(f"after ten frames that repaint a part of the page "
              f"({ticked}), the toolbar is the glass over the page as "
              f"before: {got} ({want} worked out)",
              ticked is not None and got == want)
        look_changes(q, spot, lift, tint_a)

        # a drag of 450 logical pixels up, on the page - above the
        # floating toolbar, and above the bottom edge, where a drag up is
        # the phone's, the home gesture
        top = define(MOB, "MOB_BAR_H") + bar_box(PANEL[0] // K)[1] - 20
        seen = len(q.serial())
        q.touch([(180, top - 30 * i) for i in range(16)])
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
        bookmarks(q, port)
        history_and_settings(q, port, agents)
    finally:
        q.close()
        stop()


def typing(q, port):
    """A tap on the address brings the phone's keyboard up; what is typed
    on it goes there, and Enter goes to it"""
    from t_arm64apps import KB_TOP, LETTERS, PITCH, note_key, a
    seen = len(q.serial())
    bar_tap(q, None, Phone.LOGICAL[0])
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


# the menu's rows (weft.c's menu_items)
MENU_NEW, MENU_STAR, MENU_BOOKMARKS, MENU_HISTORY, MENU_SETTINGS = range(5)


def menu_row(i):
    """The middle of the menu's i-th row, on the screen: the menu is the
    whole window, its rows under its heading"""
    row = define(WEFT_SRC, "LIST_ROW")
    return (define(MOB, "MOB_BAR_H") + define(WEFT_SRC, "M_HEAD_H") +
            i * row + row // 2)


def history_and_settings(q, port, agents):
    """The menu's History lists the pages visited, newest first, and
    goes back to one; its Settings ask for the desktop site, and clear
    the history"""
    bar = define(MOB, "MOB_BAR_H")
    width = PANEL[0] // K
    cy = PHONE_H - define(MOB, "MOB_SAFE_UP") - define(WEFT_SRC, "M_BOT_H") // 2
    row, head = define(WEFT_SRC, "LIST_ROW"), define(WEFT_SRC, "M_HEAD_H")
    seen = len(q.serial())
    bar_tap(q, 3, width)
    wait_line(q, seen, "[weft] menu shown", 30)
    q.tap(width // 2, menu_row(MENU_HISTORY))
    shown = wait_line(q, seen, "[weft] history shown", 30)
    time.sleep(1.5)
    sw, sh, at = q.screen_raw()
    first = q.find_ui(16 * K, (width - 20) * K, (bar + head + 44) * K,
                      px=12 * K, screen=(sw, sh, at),
                      charset="hmtpl:/.0123456789a")
    check(f"the menu's History lists the pages visited ({shown}), the "
          f"newest first: {first!r}",
          shown is not None and first == f"10.0.2.2:{port}/tall.html")
    seen = len(q.serial())
    q.tap(width // 2, bar + head + row // 2)
    go = wait_line(q, seen, "[weft] go ", 30)
    done = wait_line(q, seen, "[weft] load finished", 120)
    check(f"and a tap on one goes back to it ({go})",
          go == f"[weft] go http://10.0.2.2:{port}/tall.html" and
          done is not None)

    seen = len(q.serial())
    bar_tap(q, 3, width)
    wait_line(q, seen, "[weft] menu shown", 30)
    q.tap(width // 2, menu_row(MENU_SETTINGS))
    wait_line(q, seen, "[weft] settings shown", 30)
    asked = len(agents)
    q.tap(width // 2, bar + head + row + row // 2)          # Desktop site
    said = wait_line(q, seen, "[weft] setting desktop site", 30)
    end = time.time() + 120
    while len(agents) == asked and time.time() < end:
        time.sleep(0.5)
    wait_line(q, seen, "[weft] load finished", 120)
    ua = agents[-1] if len(agents) > asked else ""
    check(f"Settings: the desktop site asks again as a computer does "
          f"({said}; {ua!r})", said == "[weft] setting desktop site on" and
          re.match(r"Mozilla/5\.0 \(ThreadOS 1\.0; Linux aarch64\) "
                   r".* Safari/[\d.]+ Weft/1\.0$", ua) is not None and
          "Mobile" not in ua)
    # the search engine, from the list of them
    seen = len(q.serial())
    q.tap(width // 2, bar + head + row // 2)
    wait_line(q, seen, "[weft] search engines shown", 30)
    q.tap(width // 2, bar + head + row + row // 2)          # Google
    chosen = wait_line(q, seen, "[weft] setting search engine ", 30)
    check(f"Settings: the search engine, chosen from a list ({chosen})",
          chosen == "[weft] setting search engine Google")
    seen = len(q.serial())
    q.tap(width // 2, bar + head + 4 * row + row // 2)      # Clear
    cleared = wait_line(q, seen, "[weft] cleared history and cookies", 30)
    q.tap(width - 30, bar + head // 2)                      # Done
    bar_tap(q, 3, width)
    wait_line(q, seen, "[weft] menu shown", 30)
    q.tap(width // 2, menu_row(MENU_HISTORY))
    shown = wait_line(q, seen, "[weft] history shown", 30)
    check(f"and clear the history ({cleared}; {shown})",
          cleared is not None and shown == "[weft] history shown, 0 pages")
    # words typed into the address go to the engine chosen
    from t_arm64apps import LETTERS, note_key, KB_TOP, a
    q.tap(width - 30, bar + head // 2)                      # Done
    seen = len(q.serial())
    bar_tap(q, None, width)
    wait_line(q, seen, "[winmob] window ", 30)
    for c in "kot":
        r = next(i for i, rw in enumerate(LETTERS) if c in rw)
        q.touch([note_key(r, LETTERS[r].index(c), len(LETTERS[r]))],
                settle=0.3)
    q.tap(width - 33, KB_TOP + 3 * a("NOTES_KEY_STEP") + a("NOTES_KEY_H") // 2)
    go = wait_line(q, seen, "[weft] go ", 30)
    check(f"and words typed into the address are looked up there ({go})",
          go == "[weft] go https://www.google.com/search?q=kot")


def look_changes(q, spot, lift, tint_a):
    """The phone turns light from the shade, with Weft on the screen,
    and dark again: Weft hears it each time - the toolbar's glass tinted
    towards the look's surface, the page told (prefers-color-scheme)
    without being loaded again"""
    from t_arm64apps import cc_xy
    tile = cc_xy("dark")                                # Dark mode
    for dark, surface in ((False, (0xFF, 0xFF, 0xFF)),
                          (True, (0x1C, 0x1C, 0x1E))):
        word = "dark" if dark else "light"
        seen = len(q.serial())
        q.touch([(180, 8), (180, 150), (180, 300)])
        wait_line(q, seen, "[mobile] shade (opened)", 30)
        q.tap(*tile)
        said = wait_line(q, seen, "[mobile] dark mode ", 30)
        heard = wait_line(q, seen, "[weft] look changed", 30)
        told = wait_line(q, seen, "[weft] title scheme ", 30)
        q.touch([(180, 500), (180, 350), (180, 200)])
        wait_line(q, seen, "[mobile] shade (closed)", 30)
        want = tuple(((v + ((255 - v) * lift >> 8)) * (256 - tint_a) +
                      c * tint_a) >> 8 for v, c in zip(TALL_BG, surface))
        got = None
        end = time.time() + 30
        while got != want and time.time() < end:
            time.sleep(0.5)
            sw, sh, at = q.screen_raw()
            got = at(*spot)
        check(f"the phone turned {word} from the shade ({said}): Weft hears "
              f"it ({heard}), the page is told ({told}), and the toolbar's "
              f"glass is tinted the {word} look's way, {got} ({want} worked "
              f"out)", said == f"[mobile] dark mode {'on' if dark else 'off'}"
              and heard == f"[weft] look changed, pages {word}" and
              told == f"[weft] title scheme {word}" and got == want)


def menu_label(q, i):
    """What the menu's i-th row says, read off the screen"""
    width = PANEL[0] // K
    bar, row = define(MOB, "MOB_BAR_H"), define(WEFT_SRC, "LIST_ROW")
    px = define(WEFT_SRC, "M_TEXT_PX")
    return q.find_ui(56 * K, (width - 40) * K,
                     (bar + baseline(menu_row(i) - bar - row // 2, row,
                                     px)) * K, px=px * K)


def bookmarks(q, port):
    """The menu's star keeps the page among the bookmarks, which the
    menu's Bookmarks lists and goes back to; on a page kept, the star
    takes it out again"""
    bar = define(MOB, "MOB_BAR_H")
    width = PANEL[0] // K
    row, head = define(WEFT_SRC, "LIST_ROW"), define(WEFT_SRC, "M_HEAD_H")
    page = f"http://10.0.2.2:{port}/tall.html"
    seen = len(q.serial())
    bar_tap(q, 3, width)
    wait_line(q, seen, "[weft] menu shown", 30)
    q.tap(width // 2, menu_row(MENU_STAR))
    added = wait_line(q, seen, "[weft] bookmark ", 30)
    seen = len(q.serial())
    bar_tap(q, 3, width)
    wait_line(q, seen, "[weft] menu shown", 30)
    time.sleep(1)
    label = menu_label(q, MENU_STAR)
    check(f"the menu's star bookmarks the page ({added}), and says so on it "
          f"after: {label!r}", added == f"[weft] bookmark added {page}" and
          label == "Remove bookmark")
    q.tap(width // 2, menu_row(MENU_BOOKMARKS))
    shown = wait_line(q, seen, "[weft] bookmarks shown", 30)
    time.sleep(1.5)
    sw, sh, at = q.screen_raw()
    first = q.find_ui(16 * K, (width - 60) * K, (bar + head + 44) * K,
                      px=12 * K, screen=(sw, sh, at),
                      charset="hmtpl:/.0123456789a")
    check(f"the menu's Bookmarks lists it ({shown}): {first!r}",
          shown == "[weft] bookmarks shown, 1 pages" and
          first == f"10.0.2.2:{port}/tall.html")
    seen = len(q.serial())
    q.tap(width // 2, bar + head + row // 2)
    go = wait_line(q, seen, "[weft] go ", 30)
    done = wait_line(q, seen, "[weft] load finished", 120)
    check(f"and a tap on it goes there ({go})",
          go == f"[weft] go {page}" and done is not None)
    seen = len(q.serial())
    bar_tap(q, 3, width)
    wait_line(q, seen, "[weft] menu shown", 30)
    q.tap(width // 2, menu_row(MENU_STAR))
    removed = wait_line(q, seen, "[weft] bookmark ", 30)
    bar_tap(q, 3, width)
    wait_line(q, seen, "[weft] menu shown", 30)
    q.tap(width // 2, menu_row(MENU_BOOKMARKS))
    shown = wait_line(q, seen, "[weft] bookmarks shown", 30)
    check(f"on a bookmarked page the star takes it out ({removed}; {shown})",
          removed == f"[weft] bookmark removed {page}" and
          shown == "[weft] bookmarks shown, 0 pages")
    q.tap(width - 30, bar + head // 2)                      # Done
    wait_line(q, seen, "[weft] bookmarks hidden", 30)


def tabs(q, band):
    """The toolbar's + and tabs buttons, and the cards"""
    bar = define(MOB, "MOB_BAR_H")
    width = PANEL[0] // K
    cy = PHONE_H - define(MOB, "MOB_SAFE_UP") - define(WEFT_SRC, "M_BOT_H") // 2
    seen = len(q.serial())
    bar_tap(q, 3, width)
    menu = wait_line(q, seen, "[weft] menu shown", 30)
    time.sleep(1)
    sw, sh, at = q.screen_raw()
    bar = define(MOB, "MOB_BAR_H")
    head = q.find_ui(0, width * K // 2, (bar + baseline(
        0, define(WEFT_SRC, "M_HEAD_H"), 17)) * K, px=17 * K, face="bold",
        charset="Menu", screen=(sw, sh, at))
    items = [q.find_ui(56 * K, (width - 40) * K,
                       (bar + baseline(menu_row(i) - bar -
                                       define(WEFT_SRC, "LIST_ROW") // 2,
                                       define(WEFT_SRC, "LIST_ROW"),
                                       define(WEFT_SRC, "M_TEXT_PX"))) * K,
                       px=define(WEFT_SRC, "M_TEXT_PX") * K,
                       screen=(sw, sh, at)) for i in range(5)]
    check(f"the toolbar's menu fills the window: {head!r}, {items}",
          head == "Menu" and items == ["New tab", "Add bookmark", "Bookmarks",
                                       "History", "Settings"])
    q.tap(width // 2, menu_row(MENU_NEW))
    check("the toolbar's menu opens a second tab",
          menu is not None and
          wait_line(q, seen, "[weft] tab 2 of 2", 60) is not None)
    wait_line(q, seen, "[weft] title New tab", 60)
    seen = len(q.serial())
    bar_tap(q, 2, width)
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
    bar_tap(q, 2, width)
    wait_line(q, seen, "[weft] tabs shown", 30)
    x, y, cw = card_box(1, width)
    seen = len(q.serial())
    q.tap(x + cw - 17, bar + y + bar_h // 2)
    check("and a card's x closes its tab",
          wait_line(q, seen, "[weft] tab 1 of 1", 30) is not None)
    seen = len(q.serial())
    q.tap(PANEL[0] // K - 40, cy)                            # Done
    wait_line(q, seen, "[weft] tabs hidden", 30)


if __name__ == "__main__":
    sys.exit(main())
