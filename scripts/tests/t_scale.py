#!/usr/bin/env python3
"""ThreadOS smoke test: the scale - one desktop pixel, four screen ones.

Every window, icon and status line in this system is placed in absolute
pixels, and the font is eight of them tall. That was right while a
screen was 640x480. On a laptop's own panel it is not: eight pixels is
about a millimetre and a half, and the whole desktop comes up correct
and unreadable.

So the compositor draws into a screen of its own - a LOGICAL one - and
what reaches the panel is that screen with every pixel put down as a
square. Nothing above the video driver knows: video_w() and video_h()
answer in logical pixels, and not one line of layout changed.

What is asserted here is the mechanism, at a scale asked for by hand,
because a test machine's screen is small enough that the automatic
answer is one:

  the machine says which scale it is using, and the desktop's size is
  the screen's divided by it
  the desktop is really drawn - the background fills it, the dock is
  at the bottom - so scaling did not simply blank the screen
  and every pixel really is a square: pairs of neighbouring screen
  pixels are equal, across and down, which is a thing that cannot be
  true by accident on a screen full of eight-pixel letters

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")

    try:
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
        try:
            q.wait_for("ThreadOS shell", timeout=40)

            # The scale is a setting, so it is set before the compositor
            # starts - the mode is set up once and does not change under
            # a running desktop.
            q.type_line("sudo set scale 2")
            time.sleep(1)
            saved = q.command("thread", "saved", timeout=30)
            check("the scale setting was saved", "scale = 2" in saved)

            q.type_line("desktop")
            time.sleep(7)
            q.mouse_home()
            q.mouse_goto(560, 400)
            time.sleep(2)

            seen = len(q.serial())
            q.type_line("screen")
            time.sleep(2)
            said = q.serial()[seen:]
            m = re.search(r"mode:\s+(\d+)x(\d+), [^,]+, scale (\d+)", said)
            check("the machine says what scale it draws at ('%s')"
                  % (m.group(0) if m else said[-60:].replace("\n", " ")),
                  m is not None and m.group(3) == "2")
            if not m:
                return summary()

            lw, lh = int(m.group(1)), int(m.group(2))
            w, h, at = q.screen_raw()
            check("the desktop is half the screen each way (%dx%d of %dx%d)"
                  % (lw, lh, w, h), lw == w // 2 and lh == h // 2)

            # It is drawn, not blank: the aurora - drawn for the logical
            # screen and doubled - covers it, and the dock sits on the
            # bottom of it
            # Read where nothing stands: down the right-hand side, past
            # the windows and their shadows, between the bar and the dock
            spots = [(x, y) for x in range(2 * 300, w, 6)
                     for y in range(2 * 30, 2 * 80, 6)]
            bg = sum(1 for x, y in spots
                     if at(x, y) == aurora_px(x // 2, y // 2, lw, lh))
            check("the aurora fills the screen, doubled (%d/%d samples)"
                  % (bg, len(spots)), bg == len(spots))
            tx, ty = dock_tile(0, w=lw, h=lh)
            tile = at(2 * tx, 2 * ty)
            check("the dock at the bottom (%s in the first tile's middle)"
                  % (tile,), min(tile) > 200)

            # --- and now the thing itself ------------------------------
            # A logical pixel is a square of four, so the screen has to
            # be equal in pairs - both across and down. Sampled where
            # there IS something to see: the first kernel window, with
            # its lights, its title and its button, and the bar's name.
            pairs = same = 0
            for y in list(range(0, 48, 2)) + list(range(100, 300, 2)):
                for x in range(60, 340, 2):
                    pairs += 1
                    if at(x, y) == at(x + 1, y) and at(x, y) == at(x, y + 1) \
                       and at(x, y) == at(x + 1, y + 1):
                        same += 1
            check("every desktop pixel is a square of four (%d/%d samples)"
                  % (same, pairs), pairs > 100 and same == pairs)

            # And the counter-check: without doubling, a window with
            # letters and round lights is NOT equal in pairs. Read the
            # same places one pixel off the grid - there the squares
            # straddle each other and the equality has to break.
            off_pairs = off_same = 0
            for y in list(range(1, 47, 2)) + list(range(101, 301, 2)):
                for x in range(61, 341, 2):
                    off_pairs += 1
                    if at(x, y) == at(x + 1, y) and at(x, y) == at(x, y + 1) \
                       and at(x, y) == at(x + 1, y + 1):
                        off_same += 1
            check("and a grid moved by one pixel no longer matches "
                  "(%d/%d samples equal)" % (off_same, off_pairs),
                  off_same < off_pairs)
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
