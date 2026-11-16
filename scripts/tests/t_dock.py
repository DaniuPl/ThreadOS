#!/usr/bin/env python3
"""ThreadOS smoke test: the dock on either side, and magnified.

Settings' "Desktop & Dock" page puts the dock at the bottom of the
screen, or down its left or right side ('dockpos'), and can have its
tiles grow under the pointer ('dockmag'). What is checked is what a
person sees and does:

  the Left chip in Settings moves the dock, and the Files tile - the
  very same picture - stands at the left edge, under the bar

  a tile on the side dock starts its program, and a window filling the
  screen stops where the dock begins instead of going under it

  the same on the right

  back at the bottom with magnification on, the tile under the pointer
  reaches above where the dock's tiles end, and its neighbour less far;
  with magnification off it does not

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, _kernel_define

SEC_DESK = GSET_SECTIONS.index("Desktop & Dock")
TERM_H = _kernel_define("user/gterm.c", "H0")
FILES, TERM, WEB = 0, 2, 3


def block(at, x, y, n):
    return tuple(at(x + i, y + j) for j in range(4, n - 4)
                 for i in range(4, n - 4))


def find_tile(at, pic, n, xs, ys):
    """Where the picture of a tile stands in a strip of the screen"""
    for x in xs:
        for y in ys:
            if block(at, x, y, n) == pic:
                return x, y
    return None


def wait_moved(q, side, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        if "the dock moves to the %s" % side in q.serial():
            return True
        time.sleep(0.3)
    return False


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop())
            W, H = q.size()
            q.mouse_home()
            q.mouse_goto(W // 2, H // 2)
            time.sleep(1)
            g = dock_geom(w=W, h=H)
            n = g["tile"]
            _, _, at = q.screen_raw()
            fx, fy = g["tiles"][FILES]
            files_pic = block(at, fx, fy, n)

            # Settings: the Left chip
            q.launch_dock("gsettings")
            cx = cy = None
            end = time.time() + 15
            while time.time() < end and cx is None:
                for slot in range(8):
                    x, y = cascade(slot, GSET_W + 2, GSET_H + TITLE_H + 2,
                                   w=W, h=H)
                    if q.window_title(x, y, GSET_W + 2) == "Settings":
                        cx, cy = x + 1, y + CONTENT_DY
                        break
                time.sleep(0.3)
            check("Settings opens", cx is not None)
            if cx is None:
                return summary()
            q.click(cx + GSET_SIDE_W // 2,
                    cy + GSET_SIDE_Y0 + SEC_DESK * GSET_SIDE_H +
                    GSET_SIDE_H // 2)
            time.sleep(1)
            head = q.gset_heading(cx, cy)
            check("its Desktop & Dock page ('%s')" % head,
                  head == "Desktop & Dock")
            # (Position is the page's second row, under Interface style)
            q.click(cx + GSET_PANE_X + GSET_CHIP_W // 2,
                    cy + GSET_PANE_Y0 + GSET_ROW_H + GSET_CTRL_DY +
                    GSET_CHIP_H // 2)
            time.sleep(1)
            if q.gset_asks(cx, cy):          # a setting is the admin's
                for ch in "thread\n":
                    q.sendkey(Qemu.KEYNAMES.get(ch, ch))
                    time.sleep(0.12)
            check("the Left chip moves the dock", wait_moved(q, "left"))
            q.mouse_goto(W // 2, H // 2)
            time.sleep(1.5)
            _, _, at = q.screen_raw()
            got = find_tile(at, files_pic, n, range(0, 40),
                            range(BAR_H, H // 2))
            check("the Files tile, the same picture, at the left edge under "
                  "the bar (%s)" % (got,), got is not None)
            left_end = None
            if got:
                left_end = got[0] + n
                # A tile of the side dock starts its program
                tx, ty = got[0] + n // 2, got[1] + (n + DOCK_GAP) * TERM + \
                    n // 2
                q.click(tx, ty)
                q.mouse_goto(W // 2, H - 20)
                title = ""
                end = time.time() + 15
                while time.time() < end and "Terminal" not in title:
                    time.sleep(0.5)
                    for slot in range(8):
                        x, y = cascade(slot, TERM_W + 2,
                                       TERM_H + TITLE_H + 2, w=W, h=H)
                        t = q.window_title(x, y, TERM_W + 2)
                        if "Terminal" in t:
                            title, wx, wy = t, x, y
                            break
                check("the side dock's Terminal tile opens a terminal (%s)"
                      % title, "Terminal" in title)
                if "Terminal" in title:
                    q.mouse_goto(wx + 150, wy + 4)
                    time.sleep(0.4)
                    q.double_click()
                    q.mouse_goto(W // 2, H - 20)
                    time.sleep(2)
                    _, _, at = q.screen_raw()
                    title_bg = theme("dark")["title_bg"]
                    edge = next((x for x in range(0, W // 2)
                                 if at(x, BAR_H + 3) == title_bg), None)
                    check("a window filling the screen starts right of the "
                          "dock (its title bar from x=%s; the tiles end at "
                          "%d)" % (edge, left_end),
                          edge is not None and left_end < edge < left_end + 20)
                    check("and, with no dock under it, reaches the screen's "
                          "bottom", at(W // 2, H - 3) !=
                          aurora_px(W // 2, H - 3, W, H))
                    q.sendkey("alt-f4")
                    time.sleep(2)

            # The right
            q.click_desktop()
            check("'dockpos right'", "saved" in q.sudo("set dockpos right",
                                                       "saved"))
            check("the dock moves to the right", wait_moved(q, "right"))
            q.mouse_goto(W // 2, H // 2)
            time.sleep(1.5)
            _, _, at = q.screen_raw()
            got = find_tile(at, files_pic, n, range(W - 60, W - n),
                            range(BAR_H, H // 2))
            check("the Files tile at the right edge (%s)" % (got,),
                  got is not None)

            # Back at the bottom, magnified
            q.click_desktop()
            q.sudo("set dockpos bottom", "saved")
            check("back at the bottom", wait_moved(q, "bottom"))
            g = dock_geom(n_w=1, w=W, h=H)        # the Settings window
            wx, wy = g["tiles"][WEB]
            above = (wx + n // 2, g["ty"] - 6)
            q.mouse_goto(wx + n // 2, wy + n // 2)
            time.sleep(1.5)
            _, _, at = q.screen_raw()
            webblue = lambda p: p[2] > 200 and p[0] < 140
            # (a band of rows: one row may be all of a line of the globe)
            row = lambda at: [at(above[0] + d, above[1] - e)
                              for e in range(0, 8, 2)
                              for d in range(-n // 2 + 4, n // 2 - 4)]
            plain = sum(1 for p in row(at) if webblue(p))
            q.mouse_goto(W // 2, H // 2)
            q.click_desktop()
            q.sudo("set dockmag 1", "saved")
            time.sleep(2)
            q.mouse_goto(wx + n // 2, wy + n // 2)
            time.sleep(1.5)
            _, _, at = q.screen_raw()
            big = sum(1 for p in row(at) if webblue(p))
            check("without magnification nothing of the tile stands above "
                  "the dock's tiles (%d of Web's blue pixels 6-12 px above)"
                  % plain, plain == 0)
            check("with it, the tile under the pointer grows past them "
                  "(%d)" % big, big >= 4)
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


sys.exit(main())
