#!/usr/bin/env python3
"""ThreadOS smoke test: zooming a window - its green light.

A window could be moved, resized by its corner, minimised to the dock
and closed. What it could not be is made to fill the screen - the one
thing a person does with a window they are actually working in.

Maximising is not the same as pulling the corner as far as it goes, and
the difference is the part worth testing:

  It fills the room between the menu bar and the dock and no more. They
  are how everything else is reached, so a window drawn over them would
  be a window there is no way out of. (The bar hides itself until the
  pointer reaches the top edge; it is called up there and read.)

  It REMEMBERS. The same light puts the window back exactly where it
  was - not somewhere reasonable, exactly there.

  The application is told. A window is a buffer as well as a rectangle,
  so the buffer grows and the program gets its new size. A program that
  was not told would go on drawing 330 pixels wide inside a window 638
  wide, and the rest would be an empty grey field that looks like a
  drawing bug and is in fact a missing message.

That last one is what the assertions here really turn on. The terminal
fills ALL of its content with the ground text is typed on, a shade
darker than a window's own background, so the ground is a ruler: after
maximising it must reach the right-hand side of the screen, and after
restoring it must stop where the smaller window ends.

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
# and gterm asks for TERM_W x TERM_H of content.
WX, WY = 94, 54
# The frame: the content, a line either side, and the title bar
WW, WH = TERM_W + 2, TERM_H + TITLE_H + 2
# Zoomed, the window is the room between the bar and the dock
ZY = BAR_H
# A point in the terminal's content, clear of its text, and one right
# of the window, where only a zoomed terminal reaches
IN_X, IN_Y = WX + WW - 20, WY + 200
OUT_X = 600
DARK = theme("dark")
# A pixel of the menu bar that nothing is ever drawn on, so that it is
# the bare glass: the middle of the gap the name of the window with the
# focus has to leave before the Bluetooth icon (fit_text() in
# draw_bar(), kernel/desktop.c), ten pixels clear of the icon's rune.
# Further left a long window name could reach it, and in the gaps
# between two icons it would be as little as two pixels from one.
GLASS_X = bar_show_x() - 2 * (BAR_ITEM_GAP + STAT_W) - BAR_ITEM_GAP // 2
GLASS_Y = BAR_H // 2


def screen_when(q, ok, timeout=8):
    """The screen once ok(at) holds of it - or the last one read when the
    time is up, so that the checks fail on what was really there"""
    end = time.time() + timeout
    while True:
        w, h, at = q.screen_raw()
        if ok(at) or time.time() >= end:
            return w, h, at
        time.sleep(0.3)


def title(px):
    """The title bar of the window that has the focus"""
    return near(px, theme("dark")["title_bg"], 4)


def ground(px):
    """The terminal's ground - what only the program draws."""
    return near(px, DARK["app_field"], 3)


def grey(px):
    """A window's background: what the compositor shows where the
    program has drawn nothing."""
    return near(px, DARK["win_bg"], 3)


def dock_shows(at):
    """The dock's first tile, by the white of its folder in the middle. The
    terminal is the only window, so the dock holds one window's tile
    after the programs'."""
    return min(at(*dock_tile(0, n_w=1))) > 200


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

            q.launch_dock("gterm")                # the "Term" tile
            q.mouse_goto(600, 300)

            # The window grows out of nothing and the program then fills
            # it, so its ground is waited for, at its full size
            w, h, at = screen_when(
                q, lambda at: ground(at(IN_X, IN_Y)) and dock_shows(at), 20)
            check("the application window opened", ground(at(IN_X, IN_Y)))
            # The ground stops at the small window's edge - so its
            # reaching further later means something
            check("and its ground ends inside the window, not beyond",
                  not ground(at(OUT_X, IN_Y)))
            check("outside the window is the desktop",
                  not grey(at(OUT_X, IN_Y)))
            check("the dock shows", dock_shows(at))

            # --- zoom ---------------------------------------------------
            q.click(*light_xy(WX, WY, 2))
            q.mouse_goto(600, work_bottom() - 30)

            def zoomed(at):
                return (title(at(600, ZY + 4)) and title(at(30, ZY + 4)) and
                        ground(at(300, work_bottom() - 3)) and
                        ground(at(OUT_X, ZY + 200)))
            w, h, at = screen_when(q, zoomed)
            check("the title bar runs across the whole screen, under the "
                  "menu bar", title(at(600, ZY + 4)) and title(at(30, ZY + 4)))
            check("the window reaches down to the dock",
                  ground(at(300, work_bottom() - 3)))
            # The dock is how every other window is reached
            check("the dock still shows (the window did not cover it)",
                  dock_shows(at))

            # The ruler: the ground now reaches the right side of the
            # screen, which it can only do if the program was told the
            # new width and given a buffer that big
            check("the program got the new size (its ground reaches "
                  "the right edge)", ground(at(OUT_X, ZY + 200)))

            # The menu bar hides itself while the pointer is away from
            # it (bar_visible() in kernel/desktop.c) and comes back when
            # the pointer reaches the top edge - and then the bar's own
            # glass has to be there, not the zoomed window. The pointer
            # stays well left of the pixel that is read.
            q.mouse_goto(200, 1)
            w, h, at = screen_when(
                q, lambda at: near(at(GLASS_X, GLASS_Y),
                                   bar_glass()[GLASS_Y][GLASS_X], 1), 4)
            check("and so does the menu bar, called up at the top edge "
                  "(%s)" % (at(GLASS_X, GLASS_Y),),
                  near(at(GLASS_X, GLASS_Y), bar_glass()[GLASS_Y][GLASS_X], 1))

            # --- restore ----------------------------------------------
            q.click(*light_xy(0, ZY, 2))
            q.mouse_goto(600, 300)

            def back(at):
                return (ground(at(IN_X, IN_Y)) and not grey(at(OUT_X, IN_Y))
                        and not ground(at(OUT_X, IN_Y)))
            w, h, at = screen_when(q, back)
            check("the window came back exactly where it was",
                  ground(at(IN_X, IN_Y)) and not grey(at(OUT_X, IN_Y)))
            check("the program narrowed back (the ground ends inside the "
                  "window)", not ground(at(OUT_X, IN_Y)))

            # --- dragging a zoomed window puts it back ----------------
            q.click(*light_xy(WX, WY, 2))
            q.mouse_goto(600, 300)
            w, h, at = screen_when(q, lambda at: title(at(600, ZY + 4)))
            check("the window is full screen again", title(at(600, ZY + 4)))

            # (as far down as the screen lets a window that tall go)
            ty = min(200 - 3, h - WH)
            # A pause between the steps of the drag, so that the desktop
            # sees each on a frame of its own: a press that came in the
            # same frame as a move would land where the move ended
            q.mouse_goto(300, ZY + 5)       # grab the title bar
            time.sleep(0.4)
            q.mouse_button(True)
            time.sleep(0.4)
            q.mouse_goto(320, 200)          # and pull it down
            time.sleep(0.5)
            q.mouse_button(False)
            time.sleep(0.4)
            q.mouse_goto(600, 440)

            def dropped(at):
                return (not title(at(600, ZY + 4)) and
                        title(at(320, ty + 4)) and
                        not ground(at(600, 100)) and not grey(at(600, 100)))
            w, h, at = screen_when(q, dropped)
            check("once grabbed by the bar the window stopped being full screen",
                  not title(at(600, ZY + 4)))
            check("and it followed the pointer (its title bar is where it "
                  "was dropped)", title(at(320, ty + 4)))
            check("and the desktop is where the window no longer is",
                  not ground(at(600, 100)) and not grey(at(600, 100)))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
