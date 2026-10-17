#!/usr/bin/env python3
"""ThreadOS smoke test: choosing how the desktop looks by looking at it.

'gappear' shows the theme, the background and how windows move as
pictures of themselves: the two themes as a small desktop each with a
window on it in that theme's colours, the seven backgrounds as the
backgrounds - drawn by the kernel (SYS_BG_PREVIEW), the very pixels the
desktop would draw, only smaller - and three ways of moving.

What is asserted:

  the previews are the backgrounds to the pixel: the aurora against
  its definition at the preview's size, a colour against the slope the
  desktop paints it in

  what is in use is marked, and a click on something else asks for the
  admin password - the settings are the system's - and with it the
  desktop changes at once: the light theme repaints the bar, a colour
  background replaces the aurora, 'motion' 0 is written

  and the program's own window follows the theme it switched to

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))


def define(name):
    with open(os.path.join(ROOT, "user", "gappear.c")) as fh:
        return int(re.search(r"#define\s+%s\s+(\d+)" % name, fh.read()).group(1))


W0, H0, PAD = define("W0"), define("H0"), define("PAD")
CARD_Y, CARD_W, CARD_H, CARD_GAP = (define("CARD_Y"), define("CARD_W"),
                                    define("CARD_H"), define("CARD_GAP"))
SW_Y, SW_W, SW_H, SW_GAP = (define("SW_Y"), define("SW_W"), define("SW_H"),
                            define("SW_GAP"))
MO_Y, MO_W, MO_H = define("MO_Y"), define("MO_W"), define("MO_H")
NOTE_DY = define("NOTE_DY")

WX, WY = 94, 54
CX, CY = WX + 1, WY + CONTENT_DY


def backgrounds():
    """kernel/desktop.c's colours, by their number"""
    with open(os.path.join(ROOT, "kernel", "desktop.c")) as fh:
        src = fh.read()
    body = re.search(r"backgrounds\[\] = \{(.*?)\n\};", src, re.S).group(1)
    return [tuple(int(v) for v in m)
            for m in re.findall(r"\{\s*(\d+),\s*(\d+),\s*(\d+)\s*\}", body)]


def slope(rgb, y, h):
    """A colour background's row y of h: 115% at the top, 75% at the
    floor (paint_colour)"""
    num = 115 - y * (115 - 75) // (h - 1 if h > 1 else 1)
    return tuple(min(c * num // 100, 255) for c in rgb)


def card_x(i):
    return CX + PAD + i * (CARD_W + CARD_GAP)


def sw_x(i):
    return CX + PAD + i * (SW_W + SW_GAP)


def mo_x(i):
    return CX + PAD + i * (MO_W + 8)


def status(q, t):
    """The line at the bottom of the window"""
    base = CY + H0 - NOTE_DY
    news = q.read_ui(CX + PAD, base, W0 - 2 * PAD, px=UD_PX, face="bold",
                     colours=[t["app_good"], t["app_danger"]])
    quiet = q.read_ui(CX + PAD, base, W0 - 2 * PAD, px=UD_PX, fg=t["app_dim"])
    return news if len(news) >= len(quiet) else quiet


def password(q):
    for c in "thread":
        q.sendkey(c)
        time.sleep(0.15)
    q.sendkey("ret")
    time.sleep(2)


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    dark, light = theme("dark"), theme("light")
    bgs = backgrounds()
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()
            q.mouse_goto(300, 300)
            q.type_line("run gappear")
            time.sleep(4)
            q.mouse_goto(620, 300)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            title = q.window_title(WX, WY, W0 + 2)
            check("the window opened ('%s')" % title, title == "Appearance")

            # --- the previews are the backgrounds -----------------------------
            # (four pixels in from the edge: the corners are rounded over)
            def inner(x0, y0, pw, ph):
                return [(i, j) for j in range(4, ph - 4)
                        for i in range(4, pw - 4)]
            bad = sum(1 for i, j in inner(0, 0, SW_W, SW_H)
                      if at(sw_x(0) + i, CY + SW_Y + j) !=
                      aurora_px(i, j, SW_W, SW_H))
            check("the first swatch is the aurora, to the pixel, at its own "
                  "size (%d differ)" % bad, bad == 0)
            bad = 0
            for k in range(1, len(bgs)):
                bad += sum(1 for i, j in inner(0, 0, SW_W, SW_H)
                           if at(sw_x(k) + i, CY + SW_Y + j) !=
                           slope(bgs[k], j, SW_H))
            check("the other six are the colours, in the slope the desktop "
                  "paints them in (%d pixels differ)" % bad, bad == 0)
            # The light card's background, away from its little window
            bad = sum(1 for j in range(6, CARD_H - 6) for i in range(5, 20)
                      if at(card_x(1) + i, CY + CARD_Y + j) !=
                      aurora_px(i, j, CARD_W, CARD_H, dark=False))
            check("the light theme's card stands on the light aurora (%d "
                  "differ)" % bad, bad == 0)

            # --- what is in use is marked -----------------------------------
            ring = (card_x(0) - 3, CY + CARD_Y + CARD_H // 2)
            check("the dark theme is the one marked %s" % (at(*ring),),
                  near(at(*ring), dark["accent"], 40) and
                  not near(at(card_x(1) - 3, ring[1]), dark["accent"], 40))

            # --- choosing: the light theme ----------------------------------
            q.click(card_x(1) + CARD_W // 2, CY + CARD_Y + CARD_H // 2)
            q.mouse_goto(620, 300)
            time.sleep(1)
            said = status(q, dark)
            check("a change asks for the admin password ('%s')" % said,
                  said.startswith("Admin password"))
            password(q)
            q.mouse_goto(620, 300)
            time.sleep(2)
            w, h, at = q.screen_raw()
            said = status(q, light)
            check("with it the change is written ('%s')" % said,
                  said == "Saved")
            check("the desktop is light at once - the bar's glass (%s)"
                  % (at(300, 12),),
                  near(at(300, 12), bar_glass(False)[12][300], 1))
            check("and the window follows the theme it chose",
                  near(at(CX + W0 - 3, CY + H0 - 40), light["win_bg"], 3))
            check("and now the light theme is the one marked",
                  near(at(card_x(1) - 3, ring[1]), light["accent"], 40))

            # --- a background ---------------------------------------------
            q.click(sw_x(3) + SW_W // 2, CY + SW_Y + SW_H // 2)
            time.sleep(1)
            password(q)
            q.mouse_goto(620, 300)
            time.sleep(2)
            w, h, at = q.screen_raw()
            check("the green background replaces the aurora (%s at the "
                  "right, the slope gives %s)" % (at(600, 250),
                                                  slope(bgs[3], 250, h)),
                  at(600, 250) == slope(bgs[3], 250, h))

            # --- how windows move -------------------------------------------
            q.click(mo_x(0) + MO_W // 2, CY + MO_Y + MO_H // 2)
            time.sleep(1)
            password(q)
            q.click_desktop()
            time.sleep(0.5)
            seen = len(q.serial())
            q.type_line("set")
            time.sleep(1.5)
            check("'Still' writes motion 0",
                  re.search(r"motion = 0\b", q.serial()[seen:]) is not None)
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


sys.exit(main())
