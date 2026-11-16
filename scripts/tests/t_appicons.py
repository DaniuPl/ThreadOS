#!/usr/bin/env python3
"""ThreadOS smoke test: every program of the system wears a picture.

A program the desktop has no picture for wears a tile with its first
letter (ART_LETTER in kernel/desktop.c). Code, the accounts and the
picture viewer used to be such letters - a grey C in the dock, an A in
the menu, a P for a picture opened from Files - and they are drawn
pictures now.

What a letter looks like is taken from the system itself: a program it
has no picture for (udtest) is started, and the top of its tile in the
dock - the ground above the letter - is the letter's colour. Code's
tile in the dock, the picture viewer's window tile and the Accounts and
Code entries of the menu must not be that grey ground.

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64


def far(a, b):
    return max(abs(x - y) for x, y in zip(a, b))


def tile_top(at, g, k):
    """The ground at the top of the k-th tile, above whatever is drawn
    on it"""
    tx, ty = g["tiles"][k]
    return at(tx + g["tile"] // 2, ty + 3)


def wait_windows(q, n, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        q.mouse_goto(600, 300)
        if q.dock_windows() == n:
            return True
        time.sleep(0.5)
    return False


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    q = Qemu(KERNEL, INITRD)
    try:
        check("the desktop came up", q.wait_desktop())
        w, h = q.size()
        q.mouse_home()

        # The letter, as the system draws it
        q.click_desktop()
        q.type_line("run udtest")
        check("a program with no picture opens a window",
              wait_windows(q, 1))
        g = dock_geom(n_l=DOCK_N, n_w=1, w=w, h=h)
        _, _, at = q.screen_raw()
        letter = tile_top(at, g, DOCK_N)
        tx, ty = g["tiles"][DOCK_N]
        t = g["tile"]
        ink = sum(1 for j in range(t) for i in range(t)
                  if min(at(tx + i, ty + j)) > 220)
        check("its tile is a letter: white on a grey ground (%s, %d white "
              "pixels)" % (letter, ink),
              far(letter, (letter[0],) * 3) <= 12 and ink > t)

        # Code, in the dock
        code = DOCK_N - 1
        got = tile_top(at, g, code)
        check("Code's tile in the dock is a picture, not a letter "
              "(%s against %s)" % (got, letter), far(got, letter) > 30)

        # The picture viewer, in the dock as a window. The shell's 'run'
        # waits for its program, so udtest goes first.
        q.click(194, 64)                       # udtest's title bar
        time.sleep(0.5)
        q.sendkey("alt-f4")
        check("udtest closes", wait_windows(q, 0))
        q.click_desktop()
        q.type_line("run gimg")
        check("the picture viewer opens", wait_windows(q, 1))
        g = dock_geom(n_l=DOCK_N, n_w=1, w=w, h=h)
        _, _, at = q.screen_raw()
        got = tile_top(at, g, DOCK_N)
        check("its window's tile is a picture, not a letter (%s against %s)"
              % (got, letter), far(got, letter) > 30)

        # Accounts and Code, in the menu
        q.open_menu()
        q.mouse_goto(w - 40, h // 2)
        time.sleep(1)
        _, _, at = q.screen_raw()
        top = menu_top(h=h)
        for app in ("gusers", "gcode"):
            iy = (top + MENU_PAD + MENU_ITEMS.index(app) * MENU_ROW +
                  (MENU_ROW - MENU_ICON) // 2)
            got = at(MENU_X0 + 10 + MENU_ICON // 2, iy + 2)
            check("the menu's %s wears a picture (%s against %s)"
                  % (app, got, letter), far(got, letter) > 30)
    finally:
        q.close()
    return summary()


sys.exit(main())
