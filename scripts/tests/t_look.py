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
  else's - and bringing another window forward moves all three
  a tile in the dock is a picture with round corners, and two tiles
  have different pictures
  the theme is a setting, and switching it repaints the bar, the
  windows and the background without a restart

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


def lum(px):
    return px[0] + px[1] + px[2]


def dot_of(at, j, n_w=2):
    g = dock_geom(n_w=n_w)
    tx, ty = g["tiles"][DOCK_N + j]
    return at(tx + g["tile"] // 2, ty + g["tile"] + 1 + DOCK_DOT // 2)


def wait_dots(q, front, timeout=10):
    """The screen once the dock says window 'front' (0 the terminal, 1
    the clock) has the focus and the other has not - the accent dot
    under the one, the plain dot under the other - or as it is when the
    time is up"""
    t = theme("dark")
    end = time.time() + timeout
    while True:
        _, _, at = q.screen_raw()
        if (near(dot_of(at, front), t["accent"], 50) and
                near(dot_of(at, 1 - front), t["dot"], 50)) or \
                time.time() > end:
            return at
        time.sleep(0.3)


def shell_set(q, name, value, timeout=30):
    """'sudo set' in the kernel's shell, the password given if it is
    asked for (a shell in a signed-in admin's session is not asked).
    This machine has no disk, so the setting is changed but not
    remembered (kernel/config.c) - what the kernel then holds is asked
    with 'set <name>', and handed back."""
    start = len(q.serial())
    q.type_line("sudo set %s %s" % (name, value))
    asked = False
    end = time.time() + timeout
    while time.time() < end:
        fresh = q.serial()[start:]
        if "%s = " % name in fresh or "set: " in fresh:
            break
        if "password:" in fresh and not asked:
            q.type_line("thread")
            asked = True
        time.sleep(0.3)
    out = q.command("set " + name, "%s = " % name, timeout=15)
    m = re.search(r"%s = (\S+)" % re.escape(name), out)
    return m.group(1) if m else None


# A place on the bar with nothing under it: the pointer rests here
# while the bar is read, so that the bar (which hides when it is not
# used) stays - and the pointer is not over what is read
REST = (220, BAR_H // 2)


def bar_at(q, want, timeout=10):
    """The bar's glass at (300, 12), once the pointer has called the bar
    out - at the very top edge, and kept on the bar after that - looked
    at until it is 'want' (it slides in) or the time is up"""
    q.mouse_goto(REST[0], 1)
    time.sleep(0.3)
    q.mouse_goto(*REST)
    got = None
    end = time.time() + timeout
    while time.time() < end:
        _, _, at = q.screen_raw()
        got = at(300, 12)
        if near(got, want, 1):
            break
        time.sleep(0.3)
    return got


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
            # One window at a time: which of them is the first, and so
            # where each stands, is decided by the order they come up in
            def wait_windows(n, timeout=20):
                end = time.time() + timeout
                while time.time() < end:
                    if q.dock_windows() == n:
                        return True
                    time.sleep(0.3)
                return False
            q.launch_dock("gterm")
            check("the terminal's window comes up, the first in the dock",
                  wait_windows(1))
            q.launch_menu("gclock")
            q.mouse_goto(600, 300)       # off both windows and their lights
            check("and the clock's after it", wait_windows(2))
            # A new window is the one listening. That is waited for - by
            # then both windows are drawn - and asserted: it is what the
            # click on the terminal below has to take away.
            lit = q.wait_focus(106, 66, timeout=10)
            at = wait_dots(q, 1)
            check("the clock, opened last, has the focus: its lights are "
                  "lit and its dot in the dock is the accent (%s), the "
                  "terminal's is not (%s)" % (dot_of(at, 1), dot_of(at, 0)),
                  lit and near(dot_of(at, 1), dark["accent"], 50) and
                  near(dot_of(at, 0), dark["dot"], 50))

            # --- application windows and their shadows -----------------
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
            check("after the click the first application has focus",
                  q.wait_focus(94, 54, timeout=10))
            at = wait_dots(q, 0)
            check("its title bar is the focused colour",
                  near(at(150, 58), dark["title_bg"], 3))
            # The clock's lights are under the terminal now, so the dock
            # is where its losing the focus shows: its dot is no longer
            # the accent, and the terminal's is
            check("and the clock lost it - its dot in the dock is not the "
                  "accent (%s), the terminal's is (%s)"
                  % (dot_of(at, 1), dot_of(at, 0)),
                  near(dot_of(at, 1), dark["dot"], 50) and
                  near(dot_of(at, 0), dark["accent"], 50))

            # The clock opened second, a step down and right of the
            # terminal - and smaller than it: with the terminal in front
            # nothing of it shows. A person brings it back from the dock.
            q.click(*dock_window(1, 2))
            q.mouse_goto(600, 300)
            q.wait_focus(106, 66, timeout=10)
            at = wait_dots(q, 1)
            check("a click on the clock's tile in the dock brings it forward "
                  "with the focus",
                  q.has_focus(106, 66) and not q.has_focus(94, 54))
            # --- the dock says the same thing --------------------------
            check("its dot in the dock is the accent (%s)" % (dot_of(at, 1),),
                  near(dot_of(at, 1), dark["accent"], 50))
            check("and the other window's is not (%s)" % (dot_of(at, 0),),
                  near(dot_of(at, 0), dark["dot"], 50))

            # --- a tile in the dock has a picture on it ----------------
            g = dock_geom(n_w=2)         # (the two windows are in it too)

            def tile(k):
                tx, ty = g["tiles"][k]
                return [at(tx + i, ty + j) for j in range(3, g["tile"] - 3)
                        for i in range(3, g["tile"] - 3)]
            app, clock = tile(0), tile(1)       # (files, the monitor)
            marks = sum(1 for p in app if min(p) > 230)
            check("a tile has a drawing on it (%d white pixels)" % marks,
                  marks > 40)
            check("and two tiles have different drawings", app != clock)
            tx, ty = g["tiles"][0]
            check("a tile's corners are round", at(tx, ty) != at(tx + 4, ty + 4))

            # --- the theme is a setting, and it is followed live --------
            # (set from the kernel's shell, which has the keys once no
            # window has the focus; the clock - in front now - gets it
            # back afterwards, by a click on its title bar, so that both
            # kinds of title bar are there to be repainted)
            bar_before = bar_at(q, bar_glass(True)[12][300])
            q.click_desktop()
            held = shell_set(q, "theme", "light")
            check("the kernel holds theme = light (%s)" % held,
                  held == "light")
            q.click(106 + (GCLOCK_W + 2) * 3 // 4, 70)
            q.mouse_goto(600, 300)
            # Until the repaint is seen whole - the background and both
            # title bars, the clock's with the focus the click gave it -
            # which need not all be in the first frame that has any of it
            end = time.time() + 15
            while True:
                w, h, at = q.screen_raw()
                if (at(550, 250) == aurora_px(550, 250, dark=False) and
                        near(at(150, 58), light["title_off"], 3) and
                        near(at(150, 70), light["title_bg"], 3)) or \
                        time.time() > end:
                    break
                time.sleep(0.5)
            bar = bar_at(q, bar_glass(False)[12][300])
            check("the light theme repaints the bar - the light glass, worked "
                  "out from the light aurora (%s, was %s)"
                  % (bar, bar_before),
                  near(bar, bar_glass(False)[12][300], 1))
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
