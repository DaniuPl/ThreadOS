#!/usr/bin/env python3
"""ThreadOS smoke test: many windows open at once.

The compositor used to hold six windows. Six applications, and the
seventh failed - with no message, no error and
nothing on the screen to say why. There are nine programs in the dock,
so a person could reach that wall just by clicking what is in front of
them.

Now there is room for twelve, and room is not the whole of it. Two
things had to follow, and they are what is checked here:

  The cascade must stay on the screen. Windows are placed a step down
  and to the right of one another; far enough down that step and a
  window is drawn under the dock or past the right edge - somewhere it
  can neither be seen nor grabbed.

  The dock must hold the tiles. It is as wide as what is in it, so
  more windows means smaller tiles, not tiles that walk off the end of
  the screen. And a tile has to be CLICKABLE where it is DRAWN: the
  layout is computed in one place and both the drawing and the click
  use it, which is exactly the kind of agreement that quietly breaks.

So the assertions here are about pixels and about what a click does:
every program in the dock opens a window, every window has a tile in
the dock, and a click on the LAST of those tiles - the one whose place
depends on how much the tiles had to shrink - gives that window the
focus.

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

def tiles_inside(n_w):
    """The dock with that many windows fits the screen, tiles and all"""
    g = dock_geom(n_w=n_w)
    tx, ty = g["tiles"][-1]
    return (g["x"] >= DOCK_MARGIN and tx + g["tile"] <= SCREEN_W - DOCK_MARGIN,
            g["tile"])


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

            check("the desktop starts without placeholder windows",
                  q.dock_windows() == 0)

            # --- every program in the dock opens a window --------------
            for k in range(9):
                q.launch_dock(k)
                time.sleep(2.5)
            q.mouse_goto(600, 300)         # the cursor out of the way
            time.sleep(2)

            # Nine applications, including the duplicates that the
            # launch menu opens. The former six-window ceiling is the
            # thing this count would expose.
            n = q.dock_windows()
            check("nine windows at once have their tiles in the dock "
                  "(there are %d)" % n, n == 9)
            fits, t9 = tiles_inside(9)
            check("sixteen tiles: they got smaller (%d px) so the dock "
                  "still fits on the screen" % t9, fits and t9 < DOCK_TILE_M)

            # --- a click on the LAST tile -------------------------------
            # If the drawing and the click computed the size differently,
            # the click would land on its neighbour - or on nothing.
            q.click(*dock_window(8, 9))
            q.mouse_goto(600, 300)
            time.sleep(1.5)
            check("the clicked tile (the last) gave its window the focus",
                  q.dock_focus(9) == 8)
            # The window that tile belongs to came to the top, so its
            # title bar is on the screen with its lights on. A window
            # the cascade had pushed off the edge would have none.
            # (the settings, the last one opened: slot 8 of the cascade,
            # or as far back up it as a window that tall needs to fit)
            sx, sy = cascade(8, GSET_W + 2, GSET_H + TITLE_H + 2)
            check("a window called up from the dock has its title bar on "
                  "the screen, with the focus", q.has_focus(sx, sy))

            # --- twelve, which is all the slots there are ---------------
            # The launch menu opens three more (a second copy of an
            # application is a second window), and at twelve the tiles
            # have to become smaller still - in the drawing and in the
            # click alike.
            for name in ("guiapp", "gclock", "gfiles"):
                q.launch_menu(name)
                time.sleep(2.5)
            q.mouse_goto(600, 300)
            time.sleep(2)
            n = q.dock_windows()
            check("twelve windows at once have tiles in the dock (there are "
                  "%d)" % n, n == 12)
            fits, t12 = tiles_inside(12)
            check("and the dock still fits on the screen (%d px tiles)" % t12,
                  fits and t12 < t9)

            # The last tile - the one whose place depends entirely on the
            # shared size - still answers a click
            q.click(*dock_window(11, 12))
            q.mouse_goto(600, 300)
            time.sleep(1.5)
            check("the last tile hits its own window", q.dock_focus(12) == 11)

            # --- and one in the middle, to prove it was not luck --------
            q.click(*dock_window(6, 12))
            q.mouse_goto(600, 300)
            time.sleep(1.5)
            check("a tile from the middle of the dock hits its own window too",
                  q.dock_focus(12) == 6)

            w, h, at = q.screen_raw()
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
