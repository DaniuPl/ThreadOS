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

Settings marks its sections the same way, with a square each down its
left side, and those were letters too - W, B, N, S, ... They are
pictures now: none of the squares reads as its old letter, in
the bold lettering the letters were drawn in, and no two are alike.

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, _kernel_define

_GS = "user/gsettings.c"
ICON = _kernel_define(_GS, "ICON")
OLD_LETTERS = "WBNSGA?DLMFRiU"    # (Desktop & Dock never had a letter)


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
        q.mouse_goto(w - 40, h // 2)
        q.sendkey("esc")
        time.sleep(0.5)

        # Settings' sections
        q.launch_dock("gsettings", n_w=1)    # (the viewer is open)
        at_win = None
        end = time.time() + 15
        while time.time() < end and at_win is None:
            for slot in range(8):
                x, y = cascade(slot, GSET_W + 2, GSET_H + TITLE_H + 2,
                               w=w, h=h)
                if q.window_title(x, y, GSET_W + 2) == "Settings":
                    at_win = (x + 1, y + CONTENT_DY)
                    break
            time.sleep(0.3)
        check("Settings opens", at_win is not None)
        if at_win:
            cx, cy = at_win
            q.mouse_goto(w - 20, h // 2)
            time.sleep(1)
            shot = q.screen_raw()
            _, _, at = shot
            read, pics = [], []
            for i, old in enumerate(OLD_LETTERS):
                iy = cy + GSET_SIDE_Y0 + i * GSET_SIDE_H + \
                    (GSET_SIDE_H - ICON) // 2
                read.append(q.find_ui(cx + 12, cx + 12 + ICON,
                                      ui_font_baseline_in(11, iy, ICON),
                                      charset=OLD_LETTERS, px=11,
                                      face="bold", fg=(255, 255, 255),
                                      screen=shot))
                pics.append(tuple(at(cx + 12 + a, iy + b)
                                  for b in range(2, ICON - 2)
                                  for a in range(2, ICON - 2)))
            letters = [r for r, old in zip(read, OLD_LETTERS)
                       if old != "?" and r == old]
            check("no section's square is its old letter (%s read)"
                  % (letters,), not letters)
            check("and the %d pictures are all different" % len(pics),
                  len(set(pics)) == len(pics))
    finally:
        q.close()
    return summary()


sys.exit(main())
