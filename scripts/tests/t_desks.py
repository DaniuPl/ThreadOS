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
window left behind is a window looked for. Only that window: a second
one on the same desktop stays where it is, or Shift would be a way of
moving a whole desktop. Which desktop is showing is written on the
right of the menu bar, one cell per desktop: the one being shown in
the accent colour, an empty one dimmed, so "where am I" and "is there
anything over there" are one glance rather than two.

The assertions are about all of it, and the one that matters most is
the clock's: a program on a desktop nobody is looking at is STILL
RUNNING - read just before it goes out of sight and again when it is
back, ten seconds after that first reading, its window has moved on by
about as long (six seconds at the least, which leaves room for the two
readings), and does not show the time it had when it went out of sight.

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
from ptest import near as ptest_near
from ptest import _kernel_define as define

# The cascade keeps the familiar first application position (94,54).
WX, WY = 94, 54

# The monitor, the window that stays behind: the third window opened,
# so a step further down the cascade than the terminal. Its title bar
# starts below the place clock_here() looks at.
MON_W = define("user/gmon.c", "W") + 2
MON_H = define("user/gmon.c", "H") + TITLE_H + 2
MX, MY = cascade(4, MON_W, MON_H)

# The terminal's prompt: it starts in the signed-in user's home, and the
# account the desktop set up is an administrator's ('#')
PROMPT = "/home/admin#"

# The bar hides while the pointer is away from it and stays out while
# the pointer touches the top edge - where the pointer waits whenever
# the pager is read, clear of every item on the bar.
TOP = (320, 1)

# How long the clock is left out of sight: long enough that a clock
# which stopped while nobody looked cannot pass for one that ran
AWAY = 10


def wait_until(probe, ok, timeout=10):
    """probe() asked again until ok() accepts its answer or the time is
    up - and the last answer returned either way, so that a check fails
    on what was really there"""
    end = time.time() + timeout
    got = probe()
    while not ok(got) and time.time() < end:
        time.sleep(0.3)
        got = probe()
    return got


def pager(at, w):
    """Which of the four cells on the bar is lit - the accent pill, read
    left of where the digit is written"""
    acc = theme("dark")["accent"]
    return [ptest_near(at(bar_desk_x(d) + 3, BAR_MID), acc, 10)
            for d in range(4)]


def pager_now(q):
    w, h, at = q.screen_raw()
    return pager(at, w)


def wait_pager(q, want, timeout=10):
    """The cells once 'want' is what is lit, or what is lit when the
    time is up - the pointer at the top edge, where the bar stays out"""
    q.mouse_goto(*TOP)
    return wait_until(lambda: pager_now(q), lambda lit: lit == want, timeout)


def pager_click(q, d):
    """A click on desktop d's cell: along the top edge first, which
    brings the bar out, then down onto the cell once it is there"""
    x = bar_desk_x(d) + DESK_CELL // 2
    q.mouse_goto(x, 1)
    wait_until(lambda: pager_now(q), lambda lit: lit.count(True) == 1, 5)
    q.click(x, BAR_MID)


def clock_here(at):
    """Is the clock's window on the screen? Where its title bar would be
    is the bare aurora when it is not. The place looked at is between
    the pictures of the desktop's own shortcuts (Files, Trash), which
    stand under the left of that title bar on every desktop."""
    return at(WX + 85, WY + 4) != aurora_px(WX + 85, WY + 4)


def windows_here(q):
    """What the desktop being shown holds: (how many window tiles the
    dock has, whether the clock is on the screen)"""
    n = q.dock_windows()
    w, h, at = q.screen_raw()
    return n, clock_here(at)


def is_time(text):
    return re.fullmatch(r"\d\d:\d\d:\d\d", text) is not None


def read_clock(q, timeout=10):
    """The clock's digits, read again if the screen was caught between
    the clearing of the window and the drawing of the time. The program
    clears its content and then writes into it, and a screenshot taken
    between the two shows an empty window - which is a torn frame, not
    a clock that stopped."""
    return wait_until(lambda: q.gclock_time(WX + 1, WY + CONTENT_DY),
                      is_time, timeout)


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
            lit = wait_pager(q, [True, False, False, False])
            check("the top bar shows four desktops, the first one lit (%s)"
                  % lit, lit == [True, False, False, False])

            # --- the clock, on the first desktop ------------------------
            q.launch_menu("gclock")                # the "Clock" entry
            q.mouse_goto(*TOP)
            n = wait_until(q.dock_windows, lambda n: n == 1, 15)
            first = read_clock(q)
            t_first = time.time()
            check("the clock runs on the first desktop ('%s')" % first,
                  is_time(first))
            check("and it has its tile in the dock", n == 1)

            # --- away, to the second ------------------------------------
            q.sendkey("ctrl-alt-right")
            lit = wait_pager(q, [False, True, False, False])
            check("Ctrl+Alt+arrow moves to the second desktop (%s)" % lit,
                  lit == [False, True, False, False])
            here = wait_until(lambda: windows_here(q),
                              lambda s: s == (0, False), 5)
            check("the clock is not here - not on the screen", not here[1])
            check("nor in the dock", here[0] == 0)

            # A window of its own over here, and it is the only one
            q.launch_dock("gterm", n_w=0)         # the "Term" tile
            q.mouse_goto(*TOP)
            n = wait_until(q.dock_windows, lambda n: n == 1, 15)
            check("a new window opens where you are looking", n == 1)
            tx, ty = cascade(3, TERM_W + 2, TERM_H + TITLE_H + 2)
            last = wait_until(
                lambda: q.term_rows(tx + 1, ty + CONTENT_DY)[-1:],
                lambda rows: rows == [PROMPT], 15)
            check("and it is a terminal (its prompt: %s)" % last,
                  last == [PROMPT])

            # --- back to the first --------------------------------------
            # This wait is what the test is about, not a guess at how
            # long something takes: the clock has to be out of sight a
            # while for the time it shows to say whether it ran
            time.sleep(max(0.0, t_first + AWAY - time.time()))
            q.sendkey("ctrl-alt-left")
            lit = wait_pager(q, [True, False, False, False])
            check("and back to the first", lit == [True, False, False, False])
            here = wait_until(lambda: windows_here(q),
                              lambda s: s == (1, True), 5)
            check("the clock waited where it was left (%d window(s), "
                  "clock shown: %s)" % here, here == (1, True))
            check("and the terminal stayed on the second",
                  q.term_rows(tx + 1, ty + CONTENT_DY)[-1:] != [PROMPT])

            # The heart of it: the program was RUNNING all that time.
            # Its clock moved on by about as long as the test was away,
            # give or take the reading.
            second = read_clock(q)
            away = time.time() - t_first
            gap = (seconds(second) - seconds(first)
                   if is_time(first) and is_time(second) else -1)
            check("a program on a desktop nobody is looking at kept running "
                  "(%s -> %s, %d s in %.0f s away)"
                  % (first, second, gap, away),
                  AWAY - 4 <= gap <= away + 3)

            # --- a second window, to be left behind ---------------------
            # It opens over the clock and with the focus, and the
            # clock's tile in the dock gives the focus back
            q.launch_dock("gmon", n_w=1)          # the "Monitor" tile
            q.mouse_goto(*TOP)
            mon = wait_until(
                lambda: (q.dock_windows(), q.window_title(MX, MY, MON_W)),
                lambda s: s == (2, "Monitor"), 15)
            check("a second window opens on the first desktop, by the clock "
                  "(%d windows, '%s')" % mon, mon == (2, "Monitor"))
            q.click(*dock_window(0, 2))
            q.mouse_goto(*TOP)
            check("the clock's tile gives the clock the focus back",
                  q.wait_focus(WX, WY))

            # --- taking a window along ----------------------------------
            q.sendkey("ctrl-alt-shift-right")
            lit = wait_pager(q, [False, True, False, False])
            check("Ctrl+Alt+Shift+arrow takes the window along",
                  lit == [False, True, False, False])
            # Two windows here, the terminal and the clock: the monitor,
            # which did not have the focus, would make three
            here = wait_until(lambda: windows_here(q),
                              lambda s: s == (2, True), 5)
            check("the clock is where we are - together with the terminal, "
                  "and without the monitor (%d windows)" % here[0],
                  here == (2, True))
            # It was raised last, so it lies over the terminal, and a
            # switch gives the keyboard to the window on top
            check("and it still has the focus", q.wait_focus(WX, WY))
            third = read_clock(q)
            check("and after the move it still runs (%s -> %s)"
                  % (second, third),
                  is_time(second) and is_time(third) and
                  seconds(third) > seconds(second))

            # --- and the switch is clickable ----------------------------
            pager_click(q, 3)
            lit = wait_pager(q, [False, False, False, True])
            check("a click on a cell in the bar moves to the fourth desktop",
                  lit == [False, False, False, True])
            here = wait_until(lambda: windows_here(q),
                              lambda s: s == (0, False), 5)
            check("and there is nothing there - an empty desktop",
                  here == (0, False))

            # The clock went along to the second desktop: a click on
            # that cell brings back both windows that are there
            pager_click(q, 1)
            lit = wait_pager(q, [False, True, False, False])
            here = wait_until(lambda: windows_here(q),
                              lambda s: s == (2, True), 5)
            check("and the second desktop has its clock and terminal back",
                  lit == [False, True, False, False] and here == (2, True))

            # ... and the first one was left with the window that did not
            # have the focus, and with nothing else
            pager_click(q, 0)
            lit = wait_pager(q, [True, False, False, False])
            left = wait_until(
                lambda: windows_here(q) + (q.window_title(MX, MY, MON_W),),
                lambda s: s == (1, False, "Monitor"), 5)
            check("while the first desktop, which the clock left, holds the "
                  "monitor and nothing else (%d window(s), clock shown: %s, "
                  "'%s')" % left,
                  lit == [True, False, False, False] and
                  left == (1, False, "Monitor"))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
