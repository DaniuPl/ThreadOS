#!/usr/bin/env python3
"""ThreadOS smoke test: zooming a window - its green light.

A window could be moved, resized by its corner, minimised to the dock
and closed. What it could not be is made to fill the screen - the one
thing a person does with a window they are actually working in.

Maximising is not the same as pulling the corner as far as it goes, and
the difference is the part worth testing:

  It fills the room between the menu bar and the dock and no more. They
  are how everything else is reached, so a window drawn over them would
  be a window there is no way out of.

  It REMEMBERS. The same light puts the window back exactly where it
  was - not somewhere reasonable, exactly there.

  The application is told. A window is a buffer as well as a rectangle,
  so the buffer grows and the program gets its new size. A program that
  was not told would go on drawing 330 pixels wide inside a window 638
  wide, and the rest would be an empty grey field that looks like a
  drawing bug and is in fact a missing message.

That last one is what the assertions here really turn on. 'gsettings'
draws the selected row as a blue stripe across the FULL width of its
content area, so the stripe is a ruler: after maximising it must reach
the right-hand side of the screen, and after restoring it must stop
where the smaller window ends.

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

# The window opens in slot 2: the compositor cascades at (70+12i, 30+12i)
# and gsettings asks for 360x210 of content.
WX, WY = 94, 54
# The frame: the content, a line either side, and the title bar
WW, WH = 362, 210 + TITLE_H + 2
# Zoomed, the window is the room between the bar and the dock
ZY = BAR_H
# The selected row's stripe, a little way down the content
ROW = CONTENT_DY + 24


def title(px):
    """The title bar of the window that has the focus"""
    return near(px, theme("dark")["title_bg"], 4)


def blue(px):
    """The blue of a title bar or of a selected row."""
    r, g, b = px
    return b > 120 and r < 90 and g < 90


def grey(px):
    """The grey of a window's background."""
    r, g, b = px
    return r > 200 and g > 200 and b > 200 and abs(r - g) < 12


def dock_shows(at):
    """The dock's first tile, with its white window in the middle"""
    return min(at(*dock_tile(0, n_w=3))) > 200


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")

    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()

            q.launch_dock(6)                # the "Settings" tile
            time.sleep(3)
            q.mouse_goto(600, 300)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            check("the application window opened", grey(at(WX + 200, WY + 120)))
            # The blue stripe of the selected row stops inside the small
            # window - so its reaching further later means something
            check("the marked row is visible in the window",
                  blue(at(WX + 200, WY + ROW)))
            check("and it ends inside the window, not beyond",
                  not blue(at(WX + WW + 60, WY + ROW)))
            check("outside the window is the desktop", not grey(at(600, WY + 120)))
            check("the dock shows", dock_shows(at))

            # --- zoom ---------------------------------------------------
            q.click(*light_xy(WX, WY, 2))
            time.sleep(2.5)
            q.mouse_goto(600, work_bottom() - 30)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            check("the title bar runs across the whole screen, under the "
                  "menu bar", title(at(600, ZY + 4)) and title(at(30, ZY + 4)))
            check("the window reaches down to the dock",
                  grey(at(300, work_bottom() - 3)))
            # The dock is how every other window is reached
            check("the dock still shows (the window did not cover it)",
                  dock_shows(at))
            check("and so does the menu bar",
                  near(at(300, 12), bar_glass()[12][300], 1))

            # The ruler: the selected row's stripe now reaches the right
            # side of the screen, which it can only do if the program was
            # told the new width and given a buffer that big
            check("the program got the new size (the row bar reaches "
                  "the right edge)", blue(at(600, ZY + ROW)))
            check("and it is the row, not anything else",
                  not blue(at(600, ZY + CONTENT_DY + 4)))

            # --- restore ----------------------------------------------
            q.click(*light_xy(0, ZY, 2))
            time.sleep(2.5)
            q.mouse_goto(600, 300)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            check("the window came back exactly where it was",
                  grey(at(WX + 200, WY + 120)) and
                  not grey(at(600, WY + 120)))
            check("the program narrowed back (the row bar ends "
                  "inside the window)", not blue(at(600, WY + ROW)))
            check("and that bar is still in the window",
                  blue(at(WX + 200, WY + ROW)))

            # --- dragging a zoomed window puts it back ----------------
            q.click(*light_xy(WX, WY, 2))
            time.sleep(2)
            q.mouse_goto(600, 300)
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("the window is full screen again", title(at(600, ZY + 4)))

            q.mouse_goto(300, ZY + 5)       # grab the title bar
            q.mouse_button(True)
            q.mouse_goto(320, 200)          # and pull it down
            time.sleep(0.5)
            q.mouse_button(False)
            time.sleep(1.5)
            q.mouse_goto(600, 440)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            check("once grabbed by the bar the window stopped being full screen",
                  not title(at(600, ZY + 4)))
            check("and it followed the pointer (the window is where it was dropped)",
                  grey(at(320, 260)))
            check("and the desktop is where the window no longer is",
                  not grey(at(600, 100)))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
