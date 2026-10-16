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
written at the right-hand end of the top bar, one cell per desktop:
the one being shown in the accent colour, an empty one in the quiet
grey, so "where am I" and "is there anything over there" are one
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
from ptest import check, summary, font8x8

TB_H = 18
TBL_X0 = 46
CLOCK_W = 8 * 8 + 6
DESK_N = 4
DESK_CELL = 12

# The cascade: the first application window is slot 2, at (94,54), and
# its content starts a pixel right and a title bar (10 px) down.
WX, WY = 94, 54


def near(px, v):
    return all(abs(c - v) <= 6 for c in px)


def is_win(px):
    return near(px, 227)


def is_bar(px):
    return near(px, 210)


def is_accent(px):
    r, g, b = px
    return b > 120 and r < 90 and g < 90


def pager(at, w):
    """Which of the four cells on the top bar is lit. The leftmost
    column of a cell is the one the digit never reaches, so that is
    where the colour is read."""
    x0 = w - DESK_N * DESK_CELL - 3
    return [is_accent(at(x0 + d * DESK_CELL, 5)) for d in range(DESK_N)]


def pager_x(w, d):
    return w - DESK_N * DESK_CELL - 3 + d * DESK_CELL + 5


def buttons(at, w, y):
    out, start = [], None
    for x in range(TBL_X0, w - CLOCK_W - 2):
        on_bar = is_bar(at(x, y))
        if not on_bar and start is None:
            start = x
        elif on_bar and start is not None:
            out.append((start, x - 1))
            start = None
    if start is not None:
        out.append((start, w - CLOCK_W - 3))
    return out


def clock_text(at):
    """The 'HH:MM:SS' that gclock draws in its own window: BLACK on the
    window's light grey, at (8,6) of a content area starting at
    (95,65). Dark on light is the rule everywhere in this system - the
    digits used to be green, which was drawn and could not be read."""
    x0, y0 = WX + 1 + 8, WY + 11 + 6
    font = font8x8()
    out = ""
    for c in range(8):
        pattern = []
        for y in range(8):
            bits = 0
            for x in range(8):
                p = at(x0 + c * 8 + x, y0 + y)
                if all(v < 80 for v in p):          # the digits are dark
                    bits |= 1 << x
            pattern.append(bits)
        out += font.get(tuple(pattern), "?")
    return out


def read_clock(q, tries=8):
    """The clock's digits, read again if the screen was caught between
    the clearing of the window and the drawing of the time. The program
    clears its content and then writes into it, and a screenshot taken
    between the two shows an empty window - which is a torn frame, not
    a clock that stopped."""
    text = ""
    for _ in range(tries):
        w, h, at = q.screen_raw()
        text = clock_text(at)
        if text.count(":") == 2 and "?" not in text and " " not in text:
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
            row = h - TB_H + 2
            check("the top bar shows four desktops, the first one lit",
                  pager(at, w) == [True, False, False, False])

            # --- the clock, on the first desktop ------------------------
            q.mouse_goto(24, 72)               # the "Clock" icon
            q.mouse_click()
            time.sleep(4)
            q.mouse_goto(600, 430)
            time.sleep(1.5)
            first = read_clock(q)
            w, h, at = q.screen_raw()
            check("the clock runs on the first desktop ('%s')" % first,
                  first.count(":") == 2 and "?" not in first)
            check("and it has its button on the bar",
                  len(buttons(at, w, row)) == 3)

            # --- away, to the second ------------------------------------
            q.sendkey("ctrl-alt-right")
            time.sleep(2)
            w, h, at = q.screen_raw()
            check("Ctrl+Alt+arrow moves to the second desktop",
                  pager(at, w) == [False, True, False, False])
            check("the clock is not here - not on the screen",
                  not is_win(at(WX + 40, WY + 20)))
            check("nor on the taskbar (the kernel's windows stayed too)",
                  len(buttons(at, w, row)) == 0)

            # A window of its own over here, and it is the only one
            q.mouse_goto(24, 192)              # the "Term" icon
            q.mouse_click()
            time.sleep(4)
            q.mouse_goto(600, 430)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("a new window opens where you are looking",
                  len(buttons(at, w, row)) == 1)
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
                  len(buttons(at, w, row)) == 3)
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
            spans = buttons(at, w, row)
            check("the clock is where we are - together with the terminal",
                  len(spans) == 2)
            # The terminal opened here first, so it lies over the clock;
            # its button on the bar brings the clock back to the top
            q.mouse_goto((spans[0][0] + spans[0][1]) // 2, row + 4)
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
            q.mouse_goto(pager_x(w, 3), 5)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(1.5)
            q.mouse_goto(600, 430)
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("a click on a cell in the bar moves to the fourth desktop",
                  pager(at, w) == [False, False, False, True])
            check("and there is nothing there - an empty desktop",
                  len(buttons(at, w, row)) == 0 and
                  not is_win(at(WX + 40, WY + 20)))

            q.mouse_goto(pager_x(w, 0), 5)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(1.5)
            q.mouse_goto(600, 430)
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("and the first desktop has its kernel windows back",
                  pager(at, w) == [True, False, False, False] and
                  len(buttons(at, w, row)) == 2)
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
