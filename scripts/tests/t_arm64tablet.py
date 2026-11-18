#!/usr/bin/env python3
"""ThreadOS smoke test: the ARM64 kernel on a tablet's screen, and turned.

The same kernel and shell as the phone's (t_arm64mobile, t_arm64apps),
on a ten-inch tablet's panel, 1600 x 2560, at the same 200%. A tablet
starts on its side - one whose shorter side is 600 logical pixels or
more, Android's line - and a phone upright (kmain.c):

  given the panel upright, the tablet comes up on its side: a screen of
  2560 x 1600, 1280 x 800 logical; a phone given its panel on its side
  comes up upright

  nothing in the shell is laid out for one screen: home's grid takes as
  many columns as the width has room for (mobile.c's grid_cols), the
  four apps in one row with their colours where the columns put them;
  the dock is 85% of the width, centred; the battery against the
  status bar's right edge

  Settings, Display, Portrait turns it upright - a screen of 1600 x
  2560, 800 x 1280 logical, the grid laid out again for it - and
  Landscape back on its side

and Weft on it (needs the aarch64 weft.tapp, make weft-arm64):

  a mobile browser as on the phone, larger: the page 1280 CSS pixels
  wide at a devicePixelRatio of 2, the window whole under the floating
  toolbar, and WebKit asked for pages as a tablet asks - ThreadOS 1.0,
  Android 10, Safari without "Mobile", which sites send their tablet
  pages by; a drag scrolls the page, and the screen shows it where its
  script says
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
from t_arm64mobile import apps as home_apps
from t_webkit import TALL, BAND_TOP, BAND_H, find_box, RED, BLUE
from t_arm64weft import wait_line, WEFT, bar_box

PANEL = (1600, 2560)                # as the tablet is given it, upright
SCREEN = (2560, 1600)               # as it comes up, on its side
K = 2
LW, LH = SCREEN[0] // K, SCREEN[1] // K
MOB = "kernelarm64/mobile.c"
WEFT_SRC = "apps/webkit/weft.c"


def m(name):
    return define(MOB, name)


class Tablet(Phone):
    LOGICAL = (LW, LH)


def unlock(q, lw, lh):
    q.touch([(lw // 2, lh - 80), (lw // 2, lh - lh // 4),
             (lw // 2, lh - lh // 2)])


def home(q, lw, lh, size):
    """Home at this logical size: its grid, its dock, its battery"""
    w, h, at = q.screen_raw()
    check(f"the screen is {w}x{h}", (w, h) == size)
    step, tile = m("MOB_GRID_STEP"), m("MOB_TILE")
    gx, gy = m("MOB_GRID_X"), m("MOB_GRID_Y")
    cols = (lw - gx + step - tile) // step
    apps = home_apps()
    got = [at((gx + i * step + 4) * K, (gy + tile // 2) * K)
           for i in range(len(apps))]
    check(f"home's grid: {cols} columns, the {len(apps)} apps in one row "
          f"({got})", cols >= len(apps) and got == [c for _, c in apps])
    dw = lw * m("MOB_DOCK_PCT") // 100
    dx, dy = (lw - dw) // 2, lh - m("MOB_DOCK_UP")
    mid = (dy + m("MOB_DOCK_H") // 2) * K
    inside = at((dx + 2) * K, mid), at((dx + dw - 3) * K, mid)
    outside = at((dx - 6) * K, mid), at((dx + dw + 5) * K, mid)
    check(f"the dock {dw} wide of {lw}, centred: its panel from {dx} "
          f"to {dx + dw} ({inside} inside, {outside} out)",
          inside[0] != outside[0] and inside[1] != outside[1])
    bx = lw - m("MOB_BATT_RIGHT") - m("MOB_BATT_W")
    white = (255, 255, 255)
    check("the battery against the status bar's right edge",
          at(bx * K, 8 * K) == white and
          at((bx + m("MOB_BATT_W") - 1) * K, 12 * K) == white)


def shell():
    print("--- the shell on a tablet ---")
    from t_arm64apps import row_y, MAIN
    q = Tablet(cores=2, mib=1024, append="panel=%dx%d" % PANEL)
    try:
        out = q.wait_serial("[mobile] lock screen", timeout=120)
        check("given its panel upright, the tablet comes up on its side: "
              "1280 x 800 logical pixels",
              "[mobile] %dx%d logical, %dx%d on the panel" % (LW, LH, *SCREEN)
              in out)
        unlock(q, LW, LH)
        q.wait_serial("[mobile] home", timeout=20)
        time.sleep(1)
        home(q, LW, LH, SCREEN)
        # Settings, Display, Portrait: upright
        dock = 3
        tile = m("MOB_DOCK_TILE")
        dw = LW * m("MOB_DOCK_PCT") // 100
        gap = (dw - dock * tile) // (dock + 1)
        sx = (LW - dw) // 2 + gap + 2 * (tile + gap) + tile // 2
        sy = LH - m("MOB_DOCK_UP") + m("MOB_DOCK_H") // 2
        q.tap(sx, sy)
        q.wait_serial("[mobile] Settings (dock)", timeout=20)
        q.tap(LW // 2, row_y(MAIN["display"]))
        time.sleep(0.6)
        seen = len(q.serial())
        q.tap(LW // 2, row_y(2))
        q.wait_serial("[mobile] portrait", timeout=30)
        said = q.serial()[seen:]
        check(f"Settings, Display, Portrait turns it upright: 800 x 1280",
              "[mobile] portrait, %dx%d" % (LH, LW) in said)
        q.LOGICAL = (LH, LW)
        time.sleep(1)
        w, h, at = q.screen_raw()
        check(f"a screen of 1600 x 2560 ({w}x{h})", (w, h) == PANEL)
        seen = len(q.serial())
        q.tap(LH // 2, row_y(3))
        q.wait_serial("[mobile] landscape", timeout=30)
        q.LOGICAL = (LW, LH)
        check("and Landscape on its side again",
              "[mobile] landscape, %dx%d" % (LW, LH) in q.serial()[seen:])
        q.touch([(LW // 2, LH - 20), (LW // 2, LH - 200), (LW // 2, LH - 400)])
        q.wait_serial("[mobile] home (swipe up from the edge)", timeout=20)
        time.sleep(1)
        home(q, LW, LH, SCREEN)
    finally:
        q.close()
    print("--- a phone given its panel on its side ---")
    q = Phone(cores=2, mib=1024, append="panel=1280x720")
    try:
        out = q.wait_serial("[mobile] lock screen", timeout=120)
        check("comes up upright: 360 x 640 logical pixels",
              "[mobile] 360x640 logical, 720x1280 on the panel" in out)
    finally:
        q.close()


def browser():
    print("--- Weft on a tablet ---")
    if not os.path.exists(WEFT):
        print("SKIPPED: no aarch64 weft.tapp (make weft-arm64)")
        return
    agents = []

    def tall(h, body):
        agents.append(h.headers.get("User-Agent") or "")
        return 200, [("Content-Type", "text/html; charset=utf-8")], TALL
    ROUTES["/tall.html"] = tall
    port, stop = start_http_server()
    q = Tablet(cores=4, mib=2048,
               append="panel=%dx%d run=weft,http://10.0.2.2:%d/tall.html"
               % (*PANEL, port),
               extra=["-fw_cfg", "name=opt/threados/apps/weft,file=" + WEFT,
                      "-netdev", "user,id=n0",
                      "-device", "virtio-net-device,netdev=n0"])
    try:
        q.wait_serial("[mobile] lock screen", timeout=180)
        unlock(q, LW, LH)
        size = wait_line(q, 0, "[weft] title tall ", 600)
        check(f"the page is {LW} CSS pixels wide at a devicePixelRatio of "
              f"2 ({size})", size == f"[weft] title tall {LW} {K}")
        ua = agents[0] if agents else ""
        check(f"asked for as a tablet asks, ThreadOS 1.0's: {ua!r}",
              re.match(r"Mozilla/5\.0 \(Linux; Android 10; ThreadOS 1\.0\) "
                       r"AppleWebKit/[\d.]+ \(KHTML, like Gecko\) "
                       r"Version/[\d.]+ Safari/[\d.]+ Weft/1\.0$", ua)
              is not None)
        bar = m("MOB_BAR_H")
        blue = None
        end = time.time() + 60
        while blue is None and time.time() < end:
            sw, sh, at = q.screen_raw()
            blue = find_box(at, sw, sh, BLUE)
            time.sleep(0 if blue else 1)
        oy = bar * K
        check(f"a mobile browser's: the page the window whole, its box at "
              f"20,20 CSS pixels, 400 x 200 of the panel's ({blue})",
              blue == (20 * K, oy + 20 * K, 220 * K, oy + 120 * K))
        seen = len(q.serial())
        # well above the floating toolbar, and far enough that the band
        # ends above it too
        q.touch([(LW // 2, LH - 100 - 40 * i) for i in range(16)])
        line = wait_line(q, seen, "[weft] title scrolled ", 120)
        moved = int(line.split()[-1]) if line else 0
        check(f"a drag up scrolls the page ({line})", moved >= 200)
        # (what went up past the page's top is under the band, unseen)
        top = max(oy + (BAND_TOP - moved) * K, oy)
        page_end = (LH - m("MOB_SAFE_UP")) * K
        bottom = min(oy + (BAND_TOP + BAND_H - moved) * K, page_end)
        # measured left of the floating toolbar, which may cover the
        # band's foot however far the finger scrolled
        left = bar_box(LW, LH - bar - m("MOB_SAFE_UP"))[0] * K
        want = (0, top, left, bottom) if bottom > top else None
        band = None
        end = time.time() + 60
        while time.time() < end:
            sw, sh, at = q.screen_raw()
            band = find_box(at, left, sh, RED)
            if band == want:
                break
            time.sleep(1)
        check(f"and the screen shows it {moved} CSS pixels on: the band at "
              f"{band}, where the page puts it {want}", band == want)
    finally:
        q.close()
        stop()


def main():
    print("--- ThreadOS on a tablet ---")
    why = noarm64()
    if why:
        print("SKIPPED: " + why)
        return summary()
    shell()
    browser()
    return summary()


if __name__ == "__main__":
    sys.exit(main())
