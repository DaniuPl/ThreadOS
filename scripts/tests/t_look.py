#!/usr/bin/env python3
"""ThreadOS smoke test: how the desktop looks.

The desktop was flat: a surface was one colour, two surfaces were told
apart by a single line, and the accent blue shouted which window was
listening. It worked, and it looked like a machine from a long time
ago.

What replaced it is light and glass. The background is an aurora -
soft clouds of colour over a slope. Along the top runs a menu bar the
background shows through, blurred; windows have round corners and
stand on soft shadows, with their three buttons as coloured lights; at
the bottom a dock holds the programs and the open windows. And all of
it comes in two themes, dark and light, picked by a setting - which
the desktop follows while it runs.

Appearance is not a thing to assert as 'nice', so this group asserts
the STRUCTURE of it, which is what the eye actually reads:

  the background is not a fill: hundreds of colours down one column,
  and no step between two neighbouring rows big enough to be seen
  application windows have round corners and a shadow
  which window has the focus can be SEEN: its lights are coloured, its
  title bar a shade apart and its dot in the dock the accent - nobody
  else's - and clicking another window moves all three
  a tile in the dock is a picture with round corners, and two tiles
  have different pictures
  the theme is a setting, and switching it repaints the bar, the
  windows and the background without a restart

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


def lum(px):
    return px[0] + px[1] + px[2]


def dot_of(at, j, n_w=2):
    g = dock_geom(n_w=n_w)
    tx, ty = g["tiles"][DOCK_N + j]
    return at(tx + g["tile"] // 2, ty + g["tile"] + 1 + DOCK_DOT // 2)


def main():
    slog = tempfile.mktemp(suffix=".log")
    dark, light = theme("dark"), theme("light")
    try:
        q = Qemu(KERNEL, INITRD, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()
            q.mouse_goto(300, 400)       # the cursor off what we read
            time.sleep(1.5)
            w, h, at = q.screen_raw()

            # --- the background is an aurora ---------------------------
            col = [at(600, y) for y in range(BAR_H, 420)]
            check("the background is not a fill: %d different colours down "
                  "one column" % len(set(col)), len(set(col)) > 100)
            step = max(max(abs(a[i] - b[i]) for i in range(3))
                       for a, b in zip(col, col[1:]))
            check("and nothing in it is a step (the biggest from one row to "
                  "the next: %d)" % step, step <= 4)

            check("the desktop starts without placeholder windows",
                  q.dock_windows() == 0)
            q.launch_dock(4)
            time.sleep(3)
            q.launch_dock(1)
            time.sleep(3)

            # --- application windows and their shadows -----------------
            w, h, at = q.screen_raw()
            check("the first application's corners are rounded",
                  at(94, 54) != at(98, 58))
            near_, far_ = at(124, 54 + TERM_H + TITLE_H + 2), \
                          at(124, 54 + TERM_H + TITLE_H + 20)
            check("the first application casts a shadow",
                  lum(near_) < lum(aurora_px(124, 54 + TERM_H + TITLE_H + 2))
                  and far_ == aurora_px(124, 54 + TERM_H + TITLE_H + 20))

            # --- which application is listening can be seen ------------
            q.click(150, 58)             # first window's visible title strip
            q.mouse_goto(600, 300)
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("after the click the first application has focus",
                  q.has_focus(94, 54))
            check("its title bar is the focused colour",
                  near(at(150, 58), dark["title_bg"], 3))

            q.click(280, 70)             # second window's title bar
            q.mouse_goto(600, 300)
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("a click on the second application moves the focus",
                  q.has_focus(106, 66) and not q.has_focus(94, 54))
            # --- the dock says the same thing --------------------------
            check("its dot in the dock is the accent (%s)" % (dot_of(at, 1),),
                  near(dot_of(at, 1), dark["accent"], 50))
            check("and the other window's is not (%s)" % (dot_of(at, 0),),
                  near(dot_of(at, 0), dark["dot"], 50))

            # --- a tile in the dock has a picture on it ----------------
            g = dock_geom()

            def tile(k):
                tx, ty = g["tiles"][k]
                return [at(tx + i, ty + j) for j in range(3, g["tile"] - 3)
                        for i in range(3, g["tile"] - 3)]
            app, clock = tile(0), tile(1)
            marks = sum(1 for p in app if min(p) > 230)
            check("a tile has a drawing on it (%d white pixels)" % marks,
                  marks > 40)
            check("and two tiles have different drawings", app != clock)
            tx, ty = g["tiles"][0]
            check("a tile's corners are round", at(tx, ty) != at(tx + 4, ty + 4))

            # --- the theme is a setting, and it is followed live --------
            bar_before = at(300, 12)
            q.type_line("sudo set theme light")
            time.sleep(1)
            q.type_line("thread")
            time.sleep(3)
            q.mouse_goto(600, 300)
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("the light theme repaints the bar - the light glass, worked "
                  "out from the light aurora (%s, was %s)"
                  % (at(300, 12), bar_before),
                  near(at(300, 12), bar_glass(False)[12][300], 1))
            check("and the background", at(550, 250) ==
                  aurora_px(550, 250, dark=False))
            check("and the windows (%s)" % (at(150, 63),),
                  near(at(150, 58), light["title_off"], 3) and
                  near(at(150, 70), light["title_bg"], 3))
        finally:
            q.close()
    finally:
        if os.path.exists(slog):
            os.unlink(slog)

    return summary()


sys.exit(main())
