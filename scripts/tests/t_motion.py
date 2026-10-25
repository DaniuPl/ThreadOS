#!/usr/bin/env python3
"""ThreadOS smoke test: the desktop moves smoothly, and windows go somewhere.

The compositor used to draw a frame and then sleep thirty milliseconds,
so it could never give more than about thirty frames a second, and the
dock - glass blurred out of the background, a dozen pictures - was
drawn anew in every one of them, which took most of the frame. Now the
frames have a beat of their own (fifty a second, a sleep only for what
is left of each), the dock is kept between frames while nothing it
shows or stands on changes, and the copying underneath went from a
byte to a word at a time.

And a window no longer appears out of nothing or vanishes without a
trace: it grows the last tenth of its size as it opens, shrinks into
its tile in the dock when minimised and comes back out of it. The
'motion' setting says how quickly - 0 not at all, and up to ten times
slower, which is what lets a test catch a window half way.

What is asserted:

  the compositor says what its frames cost ('screen'), and an idle
  desktop gets more of them than the old sleep ever allowed, spending
  a small share of the second drawing them

  an idle frame still sends nothing to the card - the kept dock is the
  same picture, to the pixel

  slowed down, a window that opens is caught on its way - not yet what
  it will be, and then exactly that

  a minimised window is caught on its way down, and then the desktop
  where it stood is the bare background again

  with 'motion' 0 it is gone at once
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

SETTINGS = dock_index("gsettings")  # the settings' tile
WX, WY = 94, 54                   # slot 2 of the cascade
DARK = theme("dark")


def stats(q):
    seen = len(q.serial())
    q.type_line("screen")
    time.sleep(1.5)
    said = q.serial()[seen:]
    m = re.search(r"frames:\s+(\d+) a second, (\d+)% of it drawing", said)
    rows = re.search(r"last frame:\s+(\d+) rows", said)
    if not m:
        return None, None, None
    return (int(m.group(1)), int(m.group(2)),
            int(rows.group(1)) if rows else None)


def set_motion(q, n):
    q.click_desktop()             # the keys to the shell
    time.sleep(0.5)
    q.type_line("sudo set motion %d" % n)
    time.sleep(1)
    q.type_line("thread")
    time.sleep(1.5)


def differ(a, b, w, h, y0, y1):
    """How many pixels of the band y0..y1 two pictures disagree on"""
    return sum(1 for y in range(y0, y1) for x in range(w) if a(x, y) != b(x, y))


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
            q.mouse_goto(600, 300)
            time.sleep(3)

            # --- the beat ------------------------------------------------
            # The best of three seconds: the machine running the tests
            # may be busy with other machines at any one of them
            best = (0, 100, None)
            for _ in range(3):
                fps, busy, rows = stats(q)
                if fps is not None and fps > best[0]:
                    best = (fps, busy, rows)
            fps, busy, rows = best
            check("the compositor says what its frames cost (%s a second, "
                  "%s%% drawing)" % (fps, busy), fps > 0)
            check("an idle desktop gets more frames than a fixed sleep after "
                  "each ever gave (%d a second, more than 33)" % fps, fps > 33)
            check("and spends a small share of the second drawing them (%d%%)"
                  % busy, busy < 50)
            check("an idle frame sends nothing to the card - the kept dock "
                  "is the very same picture (%s rows)" % rows, rows == 0)

            # --- opening, slowed down ------------------------------------
            set_motion(q, 10)
            g = dock_tile(SETTINGS)
            q.mouse_goto(*g)
            time.sleep(0.4)
            q.mouse_click()
            q.mouse_goto(600, 300)
            time.sleep(0.7)
            w, h, mid = q.screen_raw()
            body = (WX + 1 + GSET_W - 3, WY + 150)     # its own background
            # The way is counted in frames, and a larger window takes
            # longer frames: wait for it to arrive rather than guess
            for _ in range(30):
                w, h, end = q.screen_raw()
                if near(end(*body), DARK["win_bg"], 3):
                    break
                time.sleep(0.5)
            check("the window opened (its background %s)" % (end(*body),),
                  near(end(*body), DARK["win_bg"], 3))
            check("and on its way it was not yet that - caught half way (%s)"
                  % (mid(*body),), mid(*body) != end(*body))

            # --- minimising, slowed down ---------------------------------
            q.mouse_goto(*light_xy(WX, WY, 1))
            time.sleep(0.4)
            q.mouse_click()
            q.mouse_goto(600, 300)
            time.sleep(0.7)
            w, h, mid = q.screen_raw()
            time.sleep(4)
            w, h, end = q.screen_raw()
            d = dock_geom(n_w=3)
            moving = differ(mid, end, w, h, BAR_H, d["y"] - 20)
            check("a minimised window is seen on its way down (%d pixels "
                  "still to go)" % moving, moving > 500)
            check("and then where it stood is the bare background",
                  end(*body) == aurora_px(*body))

            # --- and with no motion at all --------------------------------
            set_motion(q, 0)
            q.mouse_goto(*dock_window(2, 3))     # its tile brings it back
            time.sleep(0.4)
            q.mouse_click()
            q.mouse_goto(600, 300)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("brought back from the dock", near(at(*body), DARK["win_bg"], 3))
            q.mouse_goto(*light_xy(WX, WY, 1))
            time.sleep(0.4)
            q.mouse_click()
            q.mouse_goto(600, 300)
            time.sleep(0.4)
            w, h, at = q.screen_raw()
            check("with 'motion' 0 a minimised window is gone at once",
                  at(*body) == aurora_px(*body))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


sys.exit(main())
