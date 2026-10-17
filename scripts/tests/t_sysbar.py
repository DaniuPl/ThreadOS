#!/usr/bin/env python3
"""ThreadOS smoke test: the right-hand end of the menu bar.

The dock at the bottom is for programs and windows. The menu bar at the
top is for what is ALWAYS true: which of the four desktops is showing, how much of
the machine is spoken for, and a button that puts every window out of
the way.

The meter is a capsule because a capsule filled to a third is a thing to SEE - "4128 KiB" is a
thing to read. The figures behind it (how much of how much, and how
many tasks) are one hover away, so the strip stays quiet until it is
asked; this test asks, and then checks the answer against something
that changed: a program started between two looks has to show up in the
count.

"Show the desktop" is checked for the thing that makes it worth having:
the second click brings back exactly the windows it hid. A window
minimised by hand stays minimised, because that was somebody's
decision, and a button that undoes a person's decisions is a button
they stop pressing.

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
from ptest import check, summary, font8x8
from ptest import near as ptest_near

def is_accent(px):
    return ptest_near(px, theme("dark")["accent"], 12)


def is_win(px):
    return all(abs(c - 227) <= 8 for c in px)


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
            q.mouse_goto(320, 300)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            lx, sx = bar_load_x(w), bar_show_x(w)

            # --- the meter ----------------------------------------------
            # Something is in use, and not everything: the filled part
            # starts at the left and the ground is still there further
            # along. A meter that reads zero on a running machine, or
            # full on an idle one, is a meter nobody would trust.
            filled = [dx for dx in range(LOAD_W) if is_accent(at(lx + dx, BAR_MID))]
            empty = [dx for dx in range(LOAD_W)
                     if not is_accent(at(lx + dx, BAR_MID))]
            check("the gauge shows something is used (%d columns)"
                  % len(filled), len(filled) >= 1)
            check("and that not everything is (%d columns free)" % len(empty),
                  len(empty) >= 10)
            check("the fill starts from the left, and is one piece",
                  filled and filled[0] <= 2 and
                  filled == list(range(filled[0], filled[-1] + 1)))

            # --- and it says what it means when asked -------------------
            # The count has to SETTLE first. Just after the desktop
            # starts there are still tasks of its own finishing (the
            # wallpaper is drawn by a program), and on a machine loaded
            # with the whole suite that takes a while - a baseline read
            # too early counts something that is about to disappear, and
            # then the number after starting a program looks unchanged.
            def hint_now():
                q.mouse_goto(lx + 20, BAR_MID)
                time.sleep(2)
                text = q.read_hint(BAR_H + 4)
                found = re.search(r"(\d+)% \((\d+)/(\d+) MiB\), (\d+) tasks",
                                  text)
                q.mouse_goto(320, 300)     # away, so the hint disappears
                return text, found

            said, m = hint_now()
            previous = None
            for _ in range(10):
                if m and previous == int(m.group(4)):
                    break
                previous = int(m.group(4)) if m else None
                time.sleep(2)
                said, m = hint_now()

            check("the hint gives the numbers ('%s')" % said, m is not None)
            if not m:
                return summary()
            pct, used, total, tasks = (int(x) for x in m.groups())
            check("and they are numbers that hold together "
                  "(%d%% z %d/%d MiB)" % (pct, used, total),
                  0 < total <= 512 and used <= total and pct <= 100)

            # --- the figures are live -----------------------------------
            # A program started between two looks has to appear in the
            # count; a number that never moves is a picture of a number.
            q.launch_dock(4)                # the "Term" tile

            # How long a program takes to start is not something to
            # guess: with the whole suite running in parallel four
            # seconds are sometimes not enough, and a test that fails
            # because the machine was busy teaches nothing. So we look
            # again until the count moves, and give up only then.
            said2 = ""
            m2 = None
            for _ in range(12):
                said2, m2 = hint_now()
                if m2 and int(m2.group(4)) > tasks:
                    break
            check("once a program starts the hint says something else "
                  "('%s')" % said2, m2 is not None)
            if m2:
                check("and there are more tasks (%d -> %d)"
                      % (tasks, int(m2.group(4))), int(m2.group(4)) > tasks)

            # --- show the desktop ---------------------------------------
            q.launch_dock(6)                # the "Settings" tile
            time.sleep(4)
            q.mouse_goto(320, 300)
            time.sleep(1.5)
            before = q.dock_windows()
            check("two windows stand on the screen (%d in the dock with the "
                  "kernel's)" % before, before == 4)

            # The settings window is at slot 3 (106,66); minimise it by
            # hand - its yellow light. It has to STAY minimised through
            # what follows.
            q.mouse_goto(*light_xy(106, 66, 1))
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(1.5)
            q.mouse_goto(320, 300)
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("a window minimised by hand goes from the screen",
                  not is_win(at(300, 200)))

            q.mouse_goto(sx + SHOW_W // 2, BAR_MID)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(2)
            q.mouse_goto(320, 300)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("'show desktop' hides the other windows",
                  not any(all(c < 60 for c in at(x, 200))
                          for x in range(100, 400, 8)))
            check("but it does not close them - their tiles stay in the dock",
                  q.dock_windows() == before)
            check("and the button itself is lit",
                  is_accent(at(sx + 3, BAR_MID)))

            q.mouse_goto(sx + SHOW_W // 2, BAR_MID)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(2)
            q.mouse_goto(320, 300)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            # The terminal is black, so its return is easy to see
            back = any(all(c < 60 for c in at(x, 200))
                       for x in range(100, 400, 8))
            check("a second click brings back what it hid", back)
            check("and a window minimised by hand stays minimised - "
                  "the button does not undo anybody else's decisions",
                  not is_win(at(300, 120)))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
