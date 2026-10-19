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
  says they are and drawn from the same picture; that two different
  programs have different pictures; that the kernel's own two windows
  share the plain window

  that a program that is running has a dot under its tile and one that
  is not has none

  that the launch menu shows the same faces, and answers the pointer

  that a tile's name comes up over it when the pointer is there - and
  goes when it leaves

  and that the dock's size is a setting ('dock' 0, 1, 2) - the tiles
  really are that large, and a tile still clicks exactly where it is
  drawn

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

CLOCK, SETTINGS = 1, 6       # their places among the programs


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
    return q.find_ui(BAR_NAME_X, BAR_NAME_X + 100,
                     ui_font_baseline_in(BAR_PX, 0, BAR_H),
                     charset="ThreadOS", px=BAR_PX, under=on_bar)


def open_two(q, want=None):
    """The clock and the settings, from the dock. Each new window adds a
    tile, and the dock - as wide as what is in it - moves."""
    q.launch_dock(CLOCK, n_w=0, want=want)
    time.sleep(2.5)
    q.launch_dock(SETTINGS, n_w=1, want=want)
    time.sleep(2.5)
    q.mouse_goto(600, 300)
    time.sleep(1.5)


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
            time.sleep(0.4)
            check("the top bar is visible while the pointer uses it",
                  bar_name(q) == "ThreadOS")
            q.mouse_goto(600, 300)
            deadline = time.time() + 6
            while time.time() < deadline and bar_name(q) == "ThreadOS":
                time.sleep(0.2)
            check("the idle top bar hides",
                  bar_name(q) != "ThreadOS")
            q.mouse_goto(LOGO_XY[0], 1)
            time.sleep(0.4)
            q.mouse_goto(300, BAR_MID)
            time.sleep(0.4)
            check("moving to the top edge reveals the bar",
                  bar_name(q) == "ThreadOS")

            open_two(q)

            g = dock_geom(n_w=2)
            w, h, at = q.screen_raw()
            win = [tile_px(at, g, DOCK_N + j) for j in range(2)]
            check("the clock's window wears the clock's picture - pixel for "
                  "pixel the same as its tile", win[0] == tile_px(at, g, CLOCK))
            check("the settings' window wears the settings' picture",
                  win[1] == tile_px(at, g, SETTINGS))
            check("the two application windows have different pictures",
                  win[0] != win[1])

            dark = theme("dark")
            running = [k for k in range(DOCK_N)
                       if near(dot_at(at, g, k), dark["dot"], 60)]
            check("the programs that run have a dot under them, the others "
                  "none (dots under %s)" % running, running == [CLOCK, SETTINGS])

            # --- the launch menu shows the same faces ----------------
            # A program that looks like two different things - a tile
            # and a word in a menu - is two things to remember.
            q.open_menu()
            q.mouse_goto(300, 300)          # the pointer off the menu
            time.sleep(1)
            w, h, at = q.screen_raw()
            top = menu_y("gclock") - MENU_ROW // 2
            ix, iy = MENU_X0 + 10, top + (MENU_ROW - MENU_ICON) // 2
            face = sum(1 for dy in range(MENU_ICON) for dx in range(MENU_ICON)
                       if min(at(ix + dx, iy + dy)) > 220)
            check("the clock's entry has the clock's face on it (%d white "
                  "pixels)" % face, face > 40)

            # The entry under the pointer answers: it wears the accent
            q.mouse_goto(MENU_X0 + 60, menu_y("gclock"))
            time.sleep(1)
            w, h, at = q.screen_raw()
            right = MENU_X0 + MENU_W - 16
            check("the entry under the pointer is highlighted %s"
                  % (at(right, menu_y("gclock")),),
                  near(at(right, menu_y("gclock")), dark["accent"], 6))
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
            time.sleep(1.5)
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
            q2 = Qemu(KERNEL, INITRD, disk2, append=TEXT_MODE,
                      serial=tempfile.mktemp(suffix=".log"))
            try:
                q2.wait_for("ThreadOS shell", timeout=40)
                q2.type_line("sudo set dock %d" % size)
                time.sleep(1)
                q2.command("thread", "saved", timeout=30)
                q2.type_line("desktop")
                time.sleep(5)
                q2.mouse_home()
                open_two(q2, want=want)

                g = dock_geom(n_w=2, want=want)
                w, h, at = q2.screen_raw()
                tx, ty = g["tiles"][0]
                t = g["tile"]
                # Asked for 'want'; a dock that would not fit the screen
                # at that size makes its tiles a little smaller
                check("the tiles are the size asked for, or as near as "
                      "fits (%d of %d)" % (t, want), want - 3 <= t <= want)
                # The first program's picture is a blue slope with a
                # white window on it: blue from one edge of the tile to
                # the other, and not one pixel further
                edge = [at(tx + 1, ty + t // 2)[2], at(tx + t - 2, ty + t // 2)[2]]
                out = [at(tx - 2, ty + t // 2)[2], at(tx + t + 1, ty + t // 2)[2]]
                check("the first program's picture fills its tile exactly "
                      "(blue %s at its edges, %s just outside)" % (edge, out),
                      min(edge) > 240 and max(out) < 230 and
                      min(at(tx + t // 2, ty + t // 2 + 2)) > 200)
                check("a window's tile is still its program's picture",
                      tile_px(at, g, DOCK_N + 1) == tile_px(at, g, SETTINGS))

                # The one that matters: a tile clicks where it is drawn,
                # whatever size the setting made it. The Terminal goes
                # behind the settings, then its tile brings it back.
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
