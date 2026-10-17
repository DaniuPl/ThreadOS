#!/usr/bin/env python3
"""ThreadOS smoke test: virtual desktops.

Four desktops, and a window belongs to exactly one of them. It is the
oldest answer there is to a screen that has run out of room: instead
of a dozen windows in one heap, four small heaps that are switched
between - and switching costs nothing, because nothing is drawn, moved
or resized. A window on another desktop is not hidden, it is not
THERE; it keeps running and keeps drawing into its own buffer all the
same.

Ctrl+Alt with an arrow changes desktop (the arrow points where you are
going), and with Shift the window that has the focus comes along - a
window left behind is a window looked for. Which desktop is showing is
written on the right of the menu bar, one cell per desktop: the one
being shown in the accent colour, an empty one dimmed, so "where am I" and "is there anything over there" are one
glance rather than two.

The assertions are about all of it, and the one that matters most is
the clock's: a program on a desktop nobody is looking at is STILL
RUNNING - after eight seconds away its window shows a time eight
seconds later, and not the one it had when it went out of sight.

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
from ptest import near as ptest_near

# The cascade: the first application window is slot 2, at (94,54)
WX, WY = 94, 54


def near(px, v):
    return all(abs(c - v) <= 6 for c in px)


def pager(at, w):
    """Which of the four cells on the bar is lit - the accent pill, read
    left of where the digit is written"""
    acc = theme("dark")["accent"]
    return [ptest_near(at(bar_desk_x(d) + 3, BAR_MID), acc, 10)
            for d in range(4)]


def pager_x(w, d):
    return bar_desk_x(d) + DESK_CELL // 2


def clock_here(at):
    """Is the clock's window on the screen? Where its title bar would be
    is the bare aurora when it is not."""
    return at(WX + 40, WY + 4) != aurora_px(WX + 40, WY + 4)


def read_clock(q, tries=8):
    """The clock's digits, read again if the screen was caught between
    the clearing of the window and the drawing of the time. The program
    clears its content and then writes into it, and a screenshot taken
    between the two shows an empty window - which is a torn frame, not
    a clock that stopped."""
    text = ""
    for _ in range(tries):
        text = q.gclock_time(WX + 1, WY + CONTENT_DY)
        if len(text) == 8 and text.count(":") == 2:
            return text
        time.sleep(0.6)
    return text


def seconds(text):
    h, m, s = (int(p) for p in text.split(":"))
    return h * 3600 + m * 60 + s


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
            q.mouse_goto(600, 430)
            time.sleep(1)

            w, h, at = q.screen_raw()
            check("the top bar shows four desktops, the first one lit",
                  pager(at, w) == [True, False, False, False])

            # --- the clock, on the first desktop ------------------------
            q.launch_dock(1)                # the "Clock" tile
            time.sleep(4)
            q.mouse_goto(600, 430)
            time.sleep(1.5)
            first = read_clock(q)
            w, h, at = q.screen_raw()
            check("the clock runs on the first desktop ('%s')" % first,
                  first.count(":") == 2 and "?" not in first)
            check("and it has its tile in the dock",
                  q.dock_windows() == 3)

            # --- away, to the second ------------------------------------
            q.sendkey("ctrl-alt-right")
            time.sleep(2)
            w, h, at = q.screen_raw()
            check("Ctrl+Alt+arrow moves to the second desktop",
                  pager(at, w) == [False, True, False, False])
            check("the clock is not here - not on the screen",
                  not clock_here(at))
            check("nor in the dock (the kernel's windows stayed too)",
                  q.dock_windows() == 0)

            # A window of its own over here, and it is the only one
            q.launch_dock(4, n_w=0)         # the "Term" tile
            time.sleep(4)
            q.mouse_goto(600, 430)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("a new window opens where you are looking",
                  q.dock_windows() == 1)
            check("and it is a terminal (a black screen)",
                  near(at(WX + 100, WY + 60), 0))

            # --- back to the first --------------------------------------
            time.sleep(4)
            q.sendkey("ctrl-alt-left")
            time.sleep(2)
            w, h, at = q.screen_raw()
            check("and back to the first",
                  pager(at, w) == [True, False, False, False])
            check("the clock waited where it was left",
                  q.dock_windows() == 3 and clock_here(at))
            check("and the terminal stayed on the second",
                  not near(at(WX + 100, WY + 60), 0))

            # The heart of it: the program was RUNNING all that time
            second = read_clock(q)
            gap = seconds(second) - seconds(first)
            check("a program on a desktop nobody is looking at kept running "
                  "(%s -> %s, %d s)" % (first, second, gap),
                  6 <= gap <= 20)

            # --- taking a window along ----------------------------------
            q.sendkey("ctrl-alt-shift-right")
            time.sleep(2)
            w, h, at = q.screen_raw()
            check("Ctrl+Alt+Shift+arrow takes the window along",
                  pager(at, w) == [False, True, False, False])
            check("the clock is where we are - together with the terminal",
                  q.dock_windows() == 2)
            # The terminal opened here first, so it lies over the clock;
            # its tile in the dock brings the clock back to the top
            q.mouse_goto(*dock_window(0, 2))
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(2)
            q.mouse_goto(600, 430)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            third = read_clock(q)
            check("and after the move it still runs (%s -> %s)"
                  % (second, third),
                  third.count(":") == 2 and "?" not in third and
                  seconds(third) > seconds(second))

            # --- and the switch is clickable ----------------------------
            q.mouse_goto(pager_x(w, 3), BAR_MID)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(1.5)
            q.mouse_goto(600, 430)
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("a click on a cell in the bar moves to the fourth desktop",
                  pager(at, w) == [False, False, False, True])
            check("and there is nothing there - an empty desktop",
                  q.dock_windows() == 0 and not clock_here(at))

            q.mouse_goto(pager_x(w, 0), BAR_MID)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(1.5)
            q.mouse_goto(600, 430)
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("and the first desktop has its kernel windows back",
                  pager(at, w) == [True, False, False, False] and
                  q.dock_windows() == 2)
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
