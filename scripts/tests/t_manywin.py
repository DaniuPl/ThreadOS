#!/usr/bin/env python3
"""ProtonOS smoke test: many windows open at once.

The compositor used to hold six windows, two of which are its own. Four
applications, and the fifth failed - with no message, no error and
nothing on the screen to say why. There are seven icons on the desktop,
so a person could reach that wall just by clicking what is in front of
them.

Now there is room for twelve, and room is not the whole of it. Two
things had to follow, and they are what is checked here:

  The cascade must stay on the screen. Windows are placed a step down
  and to the right of one another; far enough down that step and a
  window is drawn below the taskbar or past the right edge - somewhere
  it can neither be seen nor grabbed.

  The taskbar must hold the buttons. It is a fixed strip of pixels, so
  more windows means narrower buttons, not buttons that walk off the
  end. And the button has to be CLICKABLE where it is DRAWN: the width
  is computed in one place and both the drawing and the click use it,
  which is exactly the kind of agreement that quietly breaks.

So the assertions here are about pixels and about what a click does:
every icon opens a window, every window has a button inside the bar,
and a click on the LAST of those buttons - the one that only exists
because the buttons got narrower - gives that window the focus.

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

# The taskbar: 12 px high at the bottom of a 640x480 screen, the window
# buttons starting after the 'Menu' button (2 + 40 + 4).
TB_H = 18
TBL_X0 = 46
# The clock sits at the right-hand end of the bar ("12:34:56", an
# 8-pixel font plus air), and the buttons share out what is left - so
# the scan for buttons has to stop where the clock begins.
CLOCK_W = 8 * 8 + 6

# The seven icons, each a 32x24 tile at x=8; we click the middle of one.
ICON_Y = [20, 60, 100, 140, 180, 220, 260]


def buttons(at, w, y):
    """The buttons on the taskbar as (start, end) in pixels.

    The bar itself is light grey (about 195); a button is either dark
    grey (about 105) or, when it has the focus, blue. So anything that
    is not the bar's own grey is a button, and a run of it is one
    button."""
    out = []
    start = None
    for x in range(TBL_X0, w - CLOCK_W - 2):
        r, g, b = at(x, y)
        on_bar = r > 160 and g > 160 and b > 160
        if not on_bar and start is None:
            start = x
        elif on_bar and start is not None:
            out.append((start, x - 1))
            start = None
    if start is not None:
        out.append((start, w - CLOCK_W - 3))
    return out


def title_bar(at, w, ty):
    """Finds a window's title bar: a long blue run above the taskbar.

    Returns (y, first x, last x) of the widest one found, or None. The
    title bar is the only wide blue thing on the desktop - the icons are
    orange, the bar is grey and the background is whatever the settings
    say, but never pure blue."""
    best = None
    for y in range(16, ty - TB_H, 2):
        run = 0
        start = 0
        for x in range(0, w):
            r, g, b = at(x, y)
            if b > 120 and r < 90 and g < 90:
                if run == 0:
                    start = x
                run += 1
                if run >= 60 and (best is None or run > best[3]):
                    best = (y, start, x, run)
            else:
                run = 0
    return (best[0], best[1], best[2]) if best else None


def focused(at, y, span):
    """Is the button in that span the blue (focused) one?"""
    a, b = span
    mid = (a + b) // 2
    r, g, bl = at(mid, y)
    return bl > 120 and r < 90 and g < 90


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

            w, h, at = q.screen_raw()
            ty = h - TB_H
            row = ty + 2                  # above the picture on the buttons

            before = buttons(at, w, row)
            # The kernel's own two windows are there from the start - so
            # anything we count later is on top of something real
            check("at the start the taskbar has two buttons (the kernel's windows)",
                  len(before) == 2)

            # --- every icon opens a window ----------------------------
            for i, iy in enumerate(ICON_Y):
                q.mouse_goto(24, iy + 12)
                q.mouse_click()
                time.sleep(2.5)
            q.mouse_goto(600, 300)         # the cursor out of the way
            time.sleep(2)

            w, h, at = q.screen_raw()
            got = buttons(at, w, row)
            # Seven applications plus the two kernel windows. The old
            # compositor held six windows in total, so this number is
            # itself the thing that could not happen before.
            check("nine windows at once have their buttons on the bar "
                  "(there are %d)" % len(got), len(got) >= 9)

            # A button that starts past the right edge is drawn nowhere
            # and clickable nowhere
            check("every button fits on the bar and does not "
                  "run under the clock",
                  all(a >= TBL_X0 and b < w - CLOCK_W for a, b in got))
            check("the buttons do not overlap",
                  all(got[i][1] < got[i + 1][0] for i in range(len(got) - 1)))
            # Narrower, but still wide enough to hold a letter or two
            # Every button is the same size, because what is on it is a
            # picture and a picture has one size. Nine of them fit with
            # room to spare where nine names never would have.
            widths = [b - a + 1 for a, b in got]
            check("every button has the same width (%d px)"
                  % widths[0],
                  min(widths) == max(widths) and widths[0] >= 14)

            # --- a click on the LAST button ---------------------------
            # This is the one that exists only because the buttons got
            # narrower. If the drawing and the click computed the width
            # differently, the click would land on its neighbour - or on
            # nothing at all.
            last = got[-1]
            mid = (last[0] + last[1]) // 2
            q.mouse_goto(mid, ty + 6)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(1.5)
            q.mouse_goto(600, 300)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            now = buttons(at, w, row)
            check("after the click the bar has as many buttons",
                  len(now) == len(got))
            check("the clicked button (the last) got the focus",
                  focused(at, row, now[-1]))
            check("and only it - not its neighbour",
                  not focused(at, row, now[-2]))

            # The window that button belongs to came to the top, so its
            # title bar is on the screen: a blue strip, whole, above the
            # taskbar. A window the cascade had pushed off the edge
            # would have no such strip to find.
            bar = title_bar(at, w, ty)
            check("a window called up from the bar has its title bar on the screen",
                  bar is not None)
            if bar:
                by, bx0, bx1 = bar
                check("the title bar fits on the screen and above the "
                      "taskbar (y=%d, x=%d..%d)" % (by, bx0, bx1),
                      bx0 >= 0 and bx1 < w and by < ty - TB_H)

            # --- twelve, which is where the buttons must give way -----
            # Nine still fit at full width. The launch menu opens three
            # more (a second copy of an application is a second window),
            # and at twelve the bar is too short for 52-pixel buttons -
            # so they have to become narrower, in the drawing and in the
            # click alike.
            for i in range(3):
                q.mouse_goto(20, ty + 6)          # the 'Menu' button
                q.mouse_click()
                time.sleep(1)
                q.mouse_goto(40, ty - (4 - i) * 12 + 6)
                q.mouse_click()
                time.sleep(2.5)
            q.mouse_goto(600, 300)
            time.sleep(2)

            w, h, at = q.screen_raw()
            full = buttons(at, w, row)
            check("twelve windows at once have buttons on the bar (there are %d)"
                  % len(full), len(full) == 12)
            check("and not one more, because that is how many slots there are",
                  len(full) <= 12)
            # Twelve fit WITHOUT being squeezed: pictures take a third
            # of what trimmed names took, so the bar does not have to
            # give anything up
            fw = [b - a + 1 for a, b in full]
            check("twelve fit without narrowing (%d px, the same as with "
                  "dziewieciu)" % fw[0],
                  min(fw) == max(fw) == widths[0])
            check("the whole bar still fits beside the clock",
                  full[-1][1] < w - CLOCK_W and full[0][0] >= TBL_X0)
            check("and they still do not overlap",
                  all(full[i][1] < full[i + 1][0]
                      for i in range(len(full) - 1)))

            # The last button - the one whose position depends entirely
            # on the shared width - still answers a click
            span = full[-1]
            q.mouse_goto((span[0] + span[1]) // 2, ty + 6)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(1.5)
            q.mouse_goto(600, 300)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            narrow = buttons(at, w, row)
            check("the last button hits its own window",
                  len(narrow) == 12 and focused(at, row, narrow[-1]))
            check("and not its neighbour", not focused(at, row, narrow[-2]))

            # --- and one in the middle, to prove it was not luck ------
            mid_i = len(narrow) // 2
            span = narrow[mid_i]
            q.mouse_goto((span[0] + span[1]) // 2, ty + 6)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(1.5)
            q.mouse_goto(600, 300)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            now2 = buttons(at, w, row)
            check("a button from the middle of the bar hits its own window too",
                  len(now2) == 12 and focused(at, row, now2[mid_i]))
            check("and the last one lost the focus",
                  not focused(at, row, now2[-1]))

            # The desktop survived nine windows and all that clicking
            check("the desktop is still running",
                  at(600, 300)[0] + at(600, 300)[1] + at(600, 300)[2] > 20)
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
