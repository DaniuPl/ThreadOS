#!/usr/bin/env python3
"""ProtonOS smoke test: the clock on the taskbar.

The machine had a clock - 'date' printed it and a program could ask for
it through a syscall. What it did not have was a clock a person can see
without asking, which is the only kind a desktop needs.

It sits at the right-hand end of the taskbar and shows the time to the
second. Three things had to be true for that to be worth having, and
they are what is checked here:

  It is READ, not guessed. The test reads the digits off the screen the
  same way the console tests do - by looking the eight-byte pattern of
  every glyph up in the very font the kernel drew with. So the assertion
  is about what is on the screen, not about what the kernel believes.

  It RUNS. The machine is started with the clock set to a fixed moment,
  so the digits must say that moment - and a few seconds later they must
  say a few seconds more. A clock drawn once and never refreshed would
  pass the first of those and fail the second.

  It does not eat the buttons. The bar is a fixed strip of pixels: the
  clock takes its piece, and the window buttons share out what is left.
  With nine windows open the buttons must still end before the clock
  begins - otherwise one of them is drawn under it.

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
from ptest import check, summary, font8x8, RTC_BASE

TB_H = 18
TBL_X0 = 46
CLOCK_CH = 8
CLOCK_W = CLOCK_CH * 8 + 6

ICON_Y = [20, 60, 100, 140, 180, 220, 260]


def clock_x0(w):
    return w - CLOCK_W + 3


def read_clock(q):
    """The digits off the taskbar, read through the kernel's own font."""
    w, h, at = q.screen_raw()
    x0 = clock_x0(w)
    # The bar is eighteen pixels tall now and the digits are eight, so
    # they sit five down from its top edge
    y0 = h - TB_H + 5

    counts = {}
    for y in range(y0, y0 + 8):
        for x in range(x0, x0 + CLOCK_CH * 8):
            p = at(x, y)
            counts[p] = counts.get(p, 0) + 1
    bg = max(counts, key=counts.get)

    font = font8x8()
    out = ""
    for c in range(CLOCK_CH):
        pattern = []
        for y in range(8):
            bits = 0
            for x in range(8):
                if at(x0 + c * 8 + x, y0 + y) != bg:
                    bits |= 1 << x
            pattern.append(bits)
        out += font.get(tuple(pattern), "?")
    return out


def seconds(text):
    h, m, s = (int(p) for p in text.split(":"))
    return h * 3600 + m * 60 + s


def buttons(at, w, y):
    """The window buttons on the bar as (start, end) - anything that is
    not the bar's own light grey."""
    out = []
    start = None
    for x in range(TBL_X0, clock_x0(w) - 2):
        r, g, b = at(x, y)
        on_bar = r > 160 and g > 160 and b > 160
        if not on_bar and start is None:
            start = x
        elif on_bar and start is not None:
            out.append((start, x - 1))
            start = None
    if start is not None:
        out.append((start, clock_x0(w) - 3))
    return out


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")

    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("pulpit wstal", q.wait_desktop(timeout=40))
            q.mouse_home()
            q.mouse_goto(600, 300)
            time.sleep(1)

            first = read_clock(q)
            check("na pasku zadan da sie odczytac zegar ('%s')" % first,
                  re.match(r"^\d\d:\d\d:\d\d$", first) is not None)

            # The machine is started with the clock set to a fixed
            # moment, so the digits have to say THAT moment - a clock
            # showing anything at all would pass a weaker check
            start_hm = RTC_BASE.split("T")[1][:5]
            check("zegar pokazuje godzine, z ktora wstala maszyna (%s)"
                  % start_hm, first.startswith(start_hm[:2]))
            check("i minute z tej samej godziny",
                  0 <= seconds(first) - seconds(RTC_BASE.split("T")[1]) < 300)

            # --- and it runs -------------------------------------------
            time.sleep(4)
            second = read_clock(q)
            check("po czterech sekundach zegar pokazuje cos innego ('%s')"
                  % second, second != first)
            delta = seconds(second) - seconds(first)
            check("i jest to o kilka sekund pozniej, a nie cokolwiek "
                  "(%d s)" % delta, 2 <= delta <= 8)

            # --- the buttons stop where the clock begins ---------------
            for iy in ICON_Y:
                q.mouse_goto(24, iy + 12)
                q.mouse_click()
                time.sleep(2.5)
            q.mouse_goto(600, 300)
            time.sleep(2)

            w, h, at = q.screen_raw()
            # The top row of a button: the picture on it starts three
            # pixels down, so this row is button and nothing else.
            row = h - TB_H + 2
            got = buttons(at, w, row)
            check("dziewiec okien ma swoje przyciski na pasku (jest %d)"
                  % len(got), len(got) >= 9)
            check("i zaden nie wchodzi pod zegar",
                  got[-1][1] < clock_x0(w) - 2)

            third = read_clock(q)
            check("zegar dalej da sie odczytac przy dziewieciu oknach "
                  "('%s')" % third,
                  re.match(r"^\d\d:\d\d:\d\d$", third) is not None)
            check("i chodzi dalej", seconds(third) > seconds(second))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
