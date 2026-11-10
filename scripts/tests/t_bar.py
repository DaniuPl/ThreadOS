#!/usr/bin/env python3
"""ThreadOS smoke test: the dock - its pictures, its tiles, its size.

The dock is a row of tiles at the bottom of the screen: first the
programs it starts, then one tile for every window on this desktop. A
window wears the picture of the program it was started from - the
desktop remembers, when it launches something, which tile it came
from, because a window knows its process and nothing else.

What is asserted is what a person would check, and it is checked from
the pixels:

  that a window's tile is the same picture as its program's tile -
  pixel for pixel, which is only true if both are where the layout
  says they are and drawn from the same picture; and that two different
  programs have different pictures

  that a program that is running has a dot under its tile and one that
  is not has none

  that the launch menu shows the same faces, and answers the pointer

  that a tile's name comes up over it when the pointer is there - and
  goes when it leaves

  and that the dock's size is a setting ('dock' 0, 1, 2), set in the
  kernel's shell with the desktop running - the tiles really are that
  large, and a tile still clicks exactly where it is drawn

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

MON, SETTINGS = dock_index("gmon"), dock_index("gsettings")


def tile_px(at, g, k, inset=3):
    """The inside of the k-th tile - its corners left out, they are
    blended with what is behind"""
    tx, ty = g["tiles"][k]
    t = g["tile"]
    return tuple(at(tx + i, ty + j) for j in range(inset, t - inset)
                 for i in range(inset, t - inset))


def dot_at(at, g, k):
    tx, ty = g["tiles"][k]
    return at(tx + g["tile"] // 2, ty + g["tile"] + 1 + DOCK_DOT // 2)

def bar_name(q):
    """The name at the left end of the bar - in bold (look.h, BAR_NAME_X)"""
    return q.find_ui(BAR_NAME_X, BAR_NAME_X + 100,
                     ui_font_baseline_in(BAR_PX, 0, BAR_H),
                     charset="ThreadOS", px=BAR_PX, face="bold",
                     under=on_bar)


def bar_name_shown(q, timeout=5):
    """The bar's name once it is drawn - the compositor draws the bar a
    frame or more after the pointer has come to it"""
    got = bar_name(q)
    end = time.time() + timeout
    while got != "ThreadOS" and time.time() < end:
        time.sleep(0.2)
        got = bar_name(q)
    return got


def sudo_set(q, name, value, timeout=30):
    """'sudo set' in the kernel's shell, the password given if it is
    asked for. True once the shell says the setting was saved."""
    want = "%s = %s (saved)" % (name, value)
    start = len(q.serial())
    q.type_line("sudo set %s %s" % (name, value))
    asked = False
    end = time.time() + timeout
    while time.time() < end:
        out = q.serial()[start:]
        if want in out:
            return True
        if not asked and "password:" in out:
            q.type_line("thread")
            asked = True
        time.sleep(0.3)
    return False


def screen_when(q, ok, timeout=8):
    """The screen once ok(at) holds of it - or the last one read when the
    time is up, so that the checks fail on what was really there"""
    end = time.time() + timeout
    while True:
        w, h, at = q.screen_raw()
        if ok(at) or time.time() >= end:
            return w, h, at
        time.sleep(0.3)


def windows_up(q, n, want=None, timeout=20):
    """Waits until the dock holds n window tiles"""
    end = time.time() + timeout
    while q.dock_windows(want=want) != n:
        if time.time() >= end:
            return False
        time.sleep(0.3)
    return True


def open_two(q, want=None):
    """The monitor and the settings, from the dock. Each new window adds a
    tile, and the dock - as wide as what is in it - moves: so the second
    tile is clicked only once the first window has its tile, where the
    layout for one window puts the second program. True once both
    windows have theirs."""
    q.launch_dock(MON, n_w=0, want=want)
    one = windows_up(q, 1, want)
    q.launch_dock(SETTINGS, n_w=1, want=want)
    two = windows_up(q, 2, want)
    q.mouse_goto(600, 300)
    return one and two


def fills_tile(at, g):
    """Does the first program's picture fill its tile where the layout g
    puts it? It is a folder on a blue slope: blue from one edge of the
    tile to the other, and not one pixel further. (yes or
    no, the blue at its edges, the blue just outside)"""
    tx, ty = g["tiles"][0]
    t = g["tile"]
    edge = [at(tx + 1, ty + t // 2)[2], at(tx + t - 2, ty + t // 2)[2]]
    out = [at(tx - 2, ty + t // 2)[2], at(tx + t + 1, ty + t // 2)[2]]
    ok = (min(edge) > 240 and max(out) < 230 and
          min(at(tx + t // 2, ty + t // 2 + 2)) > 200)
    return ok, edge, out


def first_width(at, g, w):
    """How wide the first program's picture is on the screen: the run of
    its blue along the tile's middle row, out from the middle of where
    the layout g puts it (0 when that is not on it). The picture's blue
    is 240 and more, the glass around it about 80 over the aurora; the
    line is drawn well clear of either."""
    tx, ty = g["tiles"][0]
    x, y = tx + g["tile"] // 2, ty + g["tile"] // 2

    def blue(x):
        return at(x, y)[2] >= 200
    if not blue(x):
        return 0
    left = right = x
    while left > 0 and blue(left - 1):
        left -= 1
    while right < w - 1 and blue(right + 1):
        right += 1
    return right - left + 1


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")

    print("--- the pictures in the dock ---")
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()

            q.mouse_goto(LOGO_XY[0], 1)
            time.sleep(0.4)
            q.mouse_goto(300, BAR_MID)
            got = bar_name_shown(q)
            check("the top bar is visible while the pointer uses it ('%s')"
                  % got, got == "ThreadOS")
            q.mouse_goto(600, 300)
            deadline = time.time() + 6
            while time.time() < deadline and bar_name(q) == "ThreadOS":
                time.sleep(0.2)
            check("the idle top bar hides",
                  bar_name(q) != "ThreadOS")
            q.mouse_goto(LOGO_XY[0], 1)
            time.sleep(0.4)
            q.mouse_goto(300, BAR_MID)
            got = bar_name_shown(q)
            check("moving to the top edge reveals the bar ('%s')" % got,
                  got == "ThreadOS")

            check("two windows opened from the dock, a tile each",
                  open_two(q))

            g = dock_geom(n_w=2)
            dark = theme("dark")

            def dots(at):
                return [k for k in range(DOCK_N)
                        if near(dot_at(at, g, k), dark["dot"], 60)]

            # A program's tile hops for a moment after its click, and is
            # compared once the hop is over
            w, h, at = screen_when(
                q, lambda at: tile_px(at, g, DOCK_N) == tile_px(at, g, MON) and
                tile_px(at, g, DOCK_N + 1) == tile_px(at, g, SETTINGS) and
                dots(at) == [MON, SETTINGS])
            win = [tile_px(at, g, DOCK_N + j) for j in range(2)]
            check("the monitor's window wears the monitor's picture - pixel for "
                  "pixel the same as its tile", win[0] == tile_px(at, g, MON))
            check("the settings' window wears the settings' picture",
                  win[1] == tile_px(at, g, SETTINGS))
            check("the two application windows have different pictures",
                  win[0] != win[1])

            running = dots(at)
            check("the programs that run have a dot under them, the others "
                  "none (dots under %s)" % running, running == [MON, SETTINGS])

            # --- the launch menu shows the same faces ----------------
            # A program that looks like two different things - a tile
            # and a word in a menu - is two things to remember.
            q.open_menu()
            q.mouse_goto(300, 300)          # the pointer off the menu
            top = menu_y("gclock") - MENU_ROW // 2
            ix, iy = MENU_X0 + 10, top + (MENU_ROW - MENU_ICON) // 2

            def face_px(at):
                return sum(1 for dy in range(MENU_ICON)
                           for dx in range(MENU_ICON)
                           if min(at(ix + dx, iy + dy)) > 220)
            w, h, at = screen_when(q, lambda at: face_px(at) > 40)
            face = face_px(at)
            check("the clock's entry has the clock's face on it (%d white "
                  "pixels)" % face, face > 40)

            # The entry under the pointer answers: it wears the accent
            q.mouse_goto(MENU_X0 + 60, menu_y("gclock"))
            right = MENU_X0 + MENU_W - 16

            def lit(at, name):
                return near(at(right, menu_y(name)), dark["accent"], 6)
            w, h, at = screen_when(q, lambda at: lit(at, "gclock"))
            check("the entry under the pointer is highlighted %s"
                  % (at(right, menu_y("gclock")),), lit(at, "gclock"))
            check("and the other entries are not",
                  not near(at(right, menu_y("gfiles")), dark["accent"], 60))
            q.open_menu()                   # closes it

            # --- the name comes up when asked for ------------------------
            # A picture says WHICH program; it does not say which of two
            # terminals. The name comes up over the tile the pointer is
            # on, and only then.
            said = q.dock_hint(DOCK_N + 1, n_w=2)
            check("the window's name comes up over its tile ('%s')" % said,
                  said == "Settings")
            q.mouse_goto(300, 300)          # the pointer off the dock
            gone = q.read_hint(g["y"] - 6 - HINT_H)
            deadline = time.time() + 5
            while gone != "" and time.time() < deadline:
                time.sleep(0.3)
                gone = q.read_hint(g["y"] - 6 - HINT_H)
            check("and it goes when the pointer leaves ('%s')" % gone,
                  gone == "")
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    # --- the size of the dock is a setting ---------------------------
    for size, want, name in ((0, DOCK_TILE_S, "small"),
                             (2, DOCK_TILE_L, "large")):
        print("--- the dock: %s ---" % name)
        disk2 = tempfile.mktemp(suffix=".img")
        with open(disk2, "wb") as f:
            f.truncate(16 * 1024 * 1024)
        try:
            q2 = Qemu(KERNEL, INITRD, disk2,
                      serial=tempfile.mktemp(suffix=".log"))
            try:
                check("the desktop came up", q2.wait_desktop(timeout=40))
                q2.mouse_home()
                q2.click_desktop()             # the keys to the shell
                check("the shell saves the dock size %d" % size,
                      sudo_set(q2, "dock", size))
                # The dock reads the setting as it draws, so it takes the
                # new size within a frame or two - and the tiles are
                # clicked where that size puts them, so not before
                g0 = dock_geom(n_w=0, want=want)
                w, h, at = screen_when(q2, lambda at: fills_tile(at, g0)[0])
                ok, edge, out = fills_tile(at, g0)
                check("the dock takes the new size while the desktop runs "
                      "(blue %s at the first tile's edges, %s just outside)"
                      % (edge, out), ok)
                check("two windows opened from the dock, a tile each",
                      open_two(q2, want=want))

                g = dock_geom(n_w=2, want=want)
                w, h, at = screen_when(
                    q2, lambda at: fills_tile(at, g)[0] and
                    tile_px(at, g, DOCK_N + 1) == tile_px(at, g, SETTINGS))
                # Asked for 'want'; a dock that would not fit the screen
                # at that size makes its tiles a little smaller. Measured
                # on the screen, not taken from the layout above
                t = first_width(at, g, w)
                check("the tiles are the size asked for, or as near as "
                      "fits (the first %d px wide on the screen, %d asked "
                      "for)" % (t, want), want - 3 <= t <= want)
                ok, edge, out = fills_tile(at, g)
                check("the first program's picture fills its tile exactly "
                      "(blue %s at its edges, %s just outside)" % (edge, out),
                      ok)
                check("a window's tile is still its program's picture",
                      tile_px(at, g, DOCK_N + 1) == tile_px(at, g, SETTINGS))

                # The one that matters: a tile clicks where it is drawn,
                # whatever size the setting made it. The monitor went
                # behind the settings, and its tile brings it back.
                q2.click(*dock_window(0, 2, want=want))
                q2.mouse_goto(600, 300)
                check("and a click on a window's tile, where it is seen, "
                      "gives that window the focus", q2.wait_focus(94, 54))
            finally:
                q2.close()
        finally:
            if os.path.exists(disk2):
                os.unlink(disk2)

    return summary()


sys.exit(main())
