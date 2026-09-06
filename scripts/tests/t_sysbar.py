#!/usr/bin/env python3
"""ProtonOS smoke test: the system strip along the top of the screen.

The taskbar at the bottom is for windows. The strip at the top is for
what is ALWAYS true: which of the four desktops is showing, how much of
the machine is spoken for, and a button that puts every window out of
the way.

The meter is a bar because a bar is a thing to SEE - "4128 KiB" is a
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

TB_H = 18
TBL_X0 = 46
CLOCK_W = 8 * 8 + 6
DESK_N = 4
DESK_CELL = 12
LOAD_BAR = 40
SHOW_W = 12


def desk_x0(w):
    return w - DESK_N * DESK_CELL - 3


def load_x(w):
    return desk_x0(w) - LOAD_BAR - 8


def show_x(w):
    return load_x(w) - 6 - SHOW_W


def is_accent(px):
    r, g, b = px
    return b > 120 and r < 90 and g < 90


def is_ground(px):                 # COL_TBBTN_OFF, the meter's empty part
    return all(abs(c - 146) <= 8 for c in px)


def is_win(px):
    return all(abs(c - 227) <= 8 for c in px)


def is_bar(px):
    return all(abs(c - 210) <= 6 for c in px)


def hint_text(at, w, h):
    """The box that appears under the strip when the pointer rests on
    something there: found by its own grey, read by the kernel's font."""
    row = [x for x in range(w) if is_win(at(x, 14))]
    if not row:
        return ""
    # row[0] is the first pixel of the FILL; the box's own border is
    # the column before it, and the text starts four pixels past the
    # border. One pixel out and every letter is unreadable.
    x0, x1 = row[0] - 1, row[-1] + 1
    font = font8x8()
    out = ""
    for c in range((x1 - x0 - 8) // 8):
        pattern = []
        for y in range(8):
            bits = 0
            for x in range(8):
                if not is_win(at(x0 + 4 + c * 8 + x, 15 + y)):
                    bits |= 1 << x
            pattern.append(bits)
        out += font.get(tuple(pattern), " " if not any(pattern) else "?")
    return out.strip()


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
            row = h - TB_H + 2
            lx, sx = load_x(w), show_x(w)

            # --- the meter ----------------------------------------------
            # Something is in use, and not everything: the filled part
            # starts at the left and the ground is still there further
            # along. A meter that reads zero on a running machine, or
            # full on an idle one, is a meter nobody would trust.
            filled = [dx for dx in range(1, LOAD_BAR - 1)
                      if is_accent(at(lx + dx, 4))]
            empty = [dx for dx in range(1, LOAD_BAR - 1)
                     if is_ground(at(lx + dx, 4))]
            check("the gauge shows something is used (%d columns)"
                  % len(filled), len(filled) >= 1)
            check("and that not everything is (%d columns free)" % len(empty),
                  len(empty) >= 10)
            check("the fill starts from the left, inside the frame",
                  filled and filled[0] == 1 and max(filled) < LOAD_BAR - 1)

            # --- and it says what it means when asked -------------------
            # The count has to SETTLE first. Just after the desktop
            # starts there are still tasks of its own finishing (the
            # wallpaper is drawn by a program), and on a machine loaded
            # with the whole suite that takes a while - a baseline read
            # too early counts something that is about to disappear, and
            # then the number after starting a program looks unchanged.
            def hint_now():
                q.mouse_goto(lx + 20, 5)
                time.sleep(2)
                ww, hh, aat = q.screen_raw()
                text = hint_text(aat, ww, hh)
                found = re.search(r"(\d+)% \((\d+)/(\d+) MiB\), (\d+) zad",
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
            q.mouse_goto(24, 192)              # the "Term" icon
            q.mouse_click()

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
            q.mouse_goto(24, 272)              # the "Config" icon as well
            q.mouse_click()
            time.sleep(4)
            q.mouse_goto(320, 300)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            before = len(buttons(at, w, row))
            check("two windows stand on the screen (%d buttons with the "
                  "kernel's)" % before, before == 4)

            # The settings window is at slot 3 (106,66); minimise it by
            # hand - its [_] is at x+w-2*8-2. It has to STAY minimised
            # through what follows.
            q.mouse_goto(106 + 362 - 2 * 8 - 2 + 4, 66 + 5)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(1.5)
            q.mouse_goto(320, 300)
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("a window minimised by hand goes from the screen",
                  not is_win(at(300, 200)))

            q.mouse_goto(sx + 6, 5)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(2)
            q.mouse_goto(320, 300)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("'show desktop' hides the other windows",
                  not any(all(c < 60 for c in at(x, 200))
                          for x in range(100, 400, 8)))
            check("but it does not close them - the buttons stay on the bar",
                  len(buttons(at, w, row)) == before)
            check("and the button itself is lit",
                  is_accent(at(sx + 6, 4)) or is_accent(at(sx + 1, 4)))

            q.mouse_goto(sx + 6, 5)
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
