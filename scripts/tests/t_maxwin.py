#!/usr/bin/env python3
"""ThreadOS smoke test: maximising a window.

A window could be moved, resized by its corner, minimised to the bar
and closed. What it could not be is made to fill the screen - the one
thing a person does with a window they are actually working in.

Maximising is not the same as pulling the corner as far as it goes, and
the difference is the part worth testing:

  It goes down to the taskbar and no further. The bar is how every other
  window is reached, so a window drawn over it would be a window there
  is no way out of.

  It REMEMBERS. The same button puts the window back exactly where it
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

# The middle of a title bar (where its buttons are), a row of the bar
# above any lettering, and how far the content has moved down since the
# bar was ten pixels tall
BAR_MID = 1 + TITLE_H // 2
BAR_TOP = 2
DOWN = TITLE_H - 10
from ptest import check, summary

# The window opens in slot 2: the compositor cascades at (70+12i, 30+12i)
# and gsettings asks for 330x160 of content, so the frame is 332x172.
WX, WY = 94, 54
# The frame: the content, a line either side, and the title bar
WW, WH = 362, 210 + TITLE_H + 2

# The bar buttons are 8x8 in the right-hand corner of the title bar:
# [O] maximise, [_] minimise, [x] close.
BTN = 8


def max_button(x, w):
    """The middle of the [O] button of a window at x, w wide."""
    return x + w - 3 * BTN - 3 + BTN // 2


def blue(px):
    """The blue of a title bar or of a selected row."""
    r, g, b = px
    return b > 120 and r < 90 and g < 90


def grey(px):
    """The grey of a window's background."""
    r, g, b = px
    return r > 200 and g > 200 and b > 200 and abs(r - g) < 12


def orange(px):
    """A desktop icon."""
    r, g, b = px
    return r > 200 and 120 < g < 200 and b < 70


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

            q.mouse_goto(24, 272)           # the 'Config' icon
            q.mouse_click()
            time.sleep(3)
            q.mouse_goto(600, 300)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            ty = h - 12                     # the top of the taskbar
            check("the application window opened", grey(at(WX + 200, WY + 120)))
            # The blue stripe of the selected row stops inside the small
            # window - so its reaching further later means something
            # The content starts at (x+1, y+CONTENT_DY) and the selected
            # row's stripe is 20 px below that.
            check("the marked row is visible in the window",
                  blue(at(WX + 200, WY + CONTENT_DY + 20)))
            check("and it ends inside the window, not beyond",
                  not blue(at(WX + WW + 60, WY + CONTENT_DY + 20)))
            check("outside the window is the desktop", not grey(at(600, WY + 120)))
            check("the desktop icons show", orange(at(24, 30)))

            # --- maximise ---------------------------------------------
            q.mouse_goto(max_button(WX, WW), WY + BAR_MID)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(2.5)
            q.mouse_goto(600, ty - 30)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            check("the title bar runs across the whole screen",
                  blue(at(600, BAR_TOP)) and blue(at(20, BAR_TOP)))
            check("the window reaches down to the taskbar",
                  grey(at(300, ty - 3)))
            # The bar is how every other window is reached
            # x=520: past the window buttons, before the clock - the
            # bar's own grey, and nothing drawn on top of it
            check("the taskbar still shows (the window did not cover it)",
                  at(520, ty + 6)[0] > 150 and at(520, ty + 6)[1] > 150)
            check("the desktop icons are covered", not orange(at(24, 30)))

            # The ruler: the selected row's stripe now reaches the right
            # side of the screen, which it can only do if the program was
            # told the new width and given a buffer that big
            check("the program got the new size (the row bar reaches "
                  "the right edge)", blue(at(600, 36 + DOWN)))
            check("and it is not the title bar but the row",
                  not blue(at(600, 20 + DOWN)))

            # --- restore ----------------------------------------------
            q.mouse_goto(max_button(0, w), BAR_MID)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(2.5)
            q.mouse_goto(600, 300)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            check("the window came back exactly where it was",
                  grey(at(WX + 200, WY + 120)) and
                  not grey(at(600, WY + 120)))
            check("the desktop icons show again", orange(at(24, 30)))
            check("the program narrowed back (the row bar ends "
                  "inside the window)", not blue(at(600, 36 + DOWN)))
            check("and that bar is still in the window", blue(at(WX + 200, WY + CONTENT_DY + 20)))

            # --- dragging a maximised window puts it back -------------
            q.mouse_goto(max_button(WX, WW), WY + BAR_MID)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(2)
            w, h, at = q.screen_raw()
            check("the window is full screen again", blue(at(600, BAR_TOP)))

            q.mouse_goto(300, 5)            # grab the title bar
            q.mouse_button(True)
            q.mouse_goto(320, 200)          # and pull it down
            time.sleep(0.5)
            q.mouse_button(False)
            time.sleep(1.5)
            q.mouse_goto(600, 440)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            check("once grabbed by the bar the window stopped being full screen",
                  not blue(at(600, BAR_TOP)))
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
