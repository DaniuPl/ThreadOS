#!/usr/bin/env python3
"""ThreadOS smoke test: the clock on the menu bar.

The machine had a clock - 'date' printed it and a program could ask for
it through a syscall. What it did not have was a clock a person can see
without asking, which is the only kind a desktop needs.

It sits at the right-hand end of the menu bar and shows the day, the
date and the time to the second. Three things had to be true for that
to be worth having, and they are what is checked here:

  It is READ, not guessed. The test reads the letters off the screen
  the way the desktop drew them - glyph by glyph, against FreeType's
  drawing of the same font. So the assertion is about what is on the
  screen, not about what the kernel believes.

  It RUNS, and it is RIGHT. The machine is started with the clock set
  to a fixed moment, so the text must say that moment - the weekday
  worked out by the kernel included - and a few seconds later it must
  say a few seconds more. A clock drawn once and never refreshed would
  pass the first of those and fail the second.

  It does not get crowded out. With a window open for every program in
  the dock its tiles get smaller to fit, and the clock - on the bar, in
  a place of its own - is still there and still running.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import datetime
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, RTC_BASE, RTC_DATE

CHARS = ("0123456789: -ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
         "ŚśŻżŹźĆćŃńĄąĘęÓóŁł")

# Where the pointer waits while the bar is read: in the one stretch of
# the bar's left part that nothing is ever drawn on. The name of the
# window with the focus is cut BAR_ITEM_GAP short of the Bluetooth icon
# (fit_text() in draw_bar(), kernel/desktop.c), and the pointer - at
# most eleven pixels and its outline wide, at any scale (draw_cursor(),
# draw_arrow()) - fits in that gap. Further left a long window name
# could run under it.
BT_X = bar_show_x() - 2 * (BAR_ITEM_GAP + STAT_W)
QUIET_X = BT_X - BAR_ITEM_GAP + 2


def read_clock(q):
    """The clock off the bar, in the bar's lettering.

    The bar hides itself until the pointer reaches the top edge
    (bar_visible() in kernel/desktop.c), so the pointer goes there
    first, to QUIET_X - well left of the clock, so that it covers none
    of it. The clock is set against the right end of its own box, which
    starts at bar_clock_x(); the desktops' cells stand just left of that
    box, and are not read."""
    q.mouse_goto(QUIET_X, 1)
    time.sleep(0.3)
    q.mouse_goto(QUIET_X, BAR_H // 2)
    time.sleep(0.2)
    for px in range(BAR_PX, 7, -1):
        text = q.find_ui(bar_clock_x(), SCREEN_W,
                         ui_font_baseline_in(BAR_PX, 0, BAR_H), charset=CHARS,
                         px=px, under=on_bar)
        if text:
            return text
    return ""


def read_clock_as(q, pattern, timeout=10):
    """The clock once it reads as 'pattern' - a setting just saved is
    taken up on one of the next frames - or what it reads when the time
    is up"""
    end = time.time() + timeout
    text = read_clock(q)
    while not re.match(pattern, text) and time.time() < end:
        time.sleep(0.3)
        text = read_clock(q)
    return text


def sudo_set(q, key, value):
    """'sudo set key value' typed to the shell, and a wait for its word
    that the setting is saved. The shell of a session signed in as an
    administrator - the desktop's, here - runs it at once; one that is
    not asks for the password first, and gets it."""
    start = len(q.serial())
    q.type_line("sudo set %s %s" % (key, value))
    asked = False
    deadline = time.time() + 30
    while time.time() < deadline:
        said = q.serial()[start:]
        if "%s = %s (saved)" % (key, value) in said:
            break
        if not asked and "password:" in said:
            q.type_line("thread")
            asked = True
        time.sleep(0.3)


def first_tile(at, g, w):
    """The first program's tile as the screen shows it: (where it begins,
    how wide it is), measured along its middle row outwards from the
    middle of where the layout g puts it - or (-1, 0) when that is not
    on it. The picture is a folder on a blue slope, its blue 240 and more
    from one edge to the other; the dock's glass around it has about 80
    over the aurora (t_bar's fills_tile() shows both), and the line is
    drawn well clear of either."""
    tx, ty = g["tiles"][0]
    x, y = tx + g["tile"] // 2, ty + g["tile"] // 2

    def blue(x):
        return at(x, y)[2] >= 200
    if not blue(x):
        return -1, 0
    left = right = x
    while left > 0 and blue(left - 1):
        left -= 1
    while right < w - 1 and blue(right + 1):
        right += 1
    return left, right - left + 1


def first_tile_when(q, g, ok, timeout=5):
    """first_tile() once ok() accepts it, or as it is when the time is up"""
    end = time.time() + timeout
    while True:
        w, h, at = q.screen_raw()
        got = first_tile(at, g, w)
        if ok(got) or time.time() >= end:
            return got
        time.sleep(0.3)


def seconds(text):
    m = re.search(r"(\d\d):(\d\d):(\d\d)$", text)
    h, mi, s = (int(p) for p in m.groups())
    return h * 3600 + mi * 60 + s


SHAPE = r"^[A-Z][a-z]{2} \d{1,2} [A-Z][a-z]{2} +\d\d:\d\d:\d\d$"


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

            first = read_clock_as(q, SHAPE)
            check("the clock can be read off the bar ('%s')" % first,
                  re.match(SHAPE, first) is not None)

            # The machine is started with the clock set to a fixed
            # moment, so the text has to say THAT moment - a clock
            # showing anything at all would pass a weaker check
            day = datetime.date.fromisoformat(RTC_DATE)
            want = "%s %d %s" % (day.strftime("%a"), day.day,
                                 day.strftime("%b"))
            check("the clock shows the day the machine booted with ('%s')"
                  % want, first.startswith(want + " "))
            start_hm = RTC_BASE.split("T")[1][:5]
            ok = re.match(SHAPE, first) is not None
            check("and its time (%s...)" % start_hm,
                  ok and first.split()[-1].startswith(start_hm[:2]))
            check("and a minute of the same hour",
                  ok and 0 <= seconds(first) -
                  seconds(RTC_BASE.split("T")[1]) < 300)

            # --- and it runs -------------------------------------------
            # (a wait that is the point: time has to pass on the clock)
            time.sleep(4)
            second = read_clock_as(q, SHAPE)
            check("four seconds later the clock shows something else ('%s')"
                  % second, second != first)
            ok2 = ok and re.match(SHAPE, second) is not None
            delta = seconds(second) - seconds(first) if ok2 else -1
            check("and it is a few seconds later, not just anything "
                  "(%d s)" % delta, 2 <= delta <= 8)

            # --- a crowded dock --------------------------------------
            # The first tile as it is with no window open, to be measured
            # again, the same way, once the dock is full
            g0 = dock_geom(n_w=0)
            x0, t0 = first_tile_when(q, g0,
                                     lambda got: got[1] == DOCK_TILE_M)
            check("with no window open the first tile is the dock's "
                  "medium size (%d px wide at x=%d)" % (t0, x0),
                  t0 == DOCK_TILE_M)

            # Every program in the dock, one window each: k windows are
            # open when the k-th tile is clicked (launch_dock counts them).
            # Each window's tile moves the dock, so the next tile is
            # clicked only once the last window has its own.
            for k in range(DOCK_N):
                q.launch_dock(k)
                deadline = time.time() + 30
                while (q.dock_windows() != k + 1 and
                       time.time() < deadline):
                    time.sleep(0.3)
            q.mouse_goto(600, 300)

            n = q.dock_windows()
            check("%d windows are open, a tile each (there are %d)"
                  % (DOCK_N, n), n == DOCK_N)
            # At the size it had, the full dock would be wider than the
            # screen and start off its left edge. Measured on the screen:
            # the tile is narrower than before, and the dock begins its
            # margin in from the edge - and, centred, ends as far in from
            # the other, where the next check finds the last tile.
            g = dock_geom(n_w=DOCK_N)
            inside = DOCK_MARGIN + DOCK_PAD
            x1, t1 = first_tile_when(
                q, g, lambda got: 0 < got[1] < t0 and got[0] >= inside)
            check("%d windows: the dock's tiles got smaller to fit (the "
                  "first %d px wide, %d with no window) and it starts "
                  "inside the screen (at x=%d, %d at the least)"
                  % (DOCK_N, t1, t0, x1, inside),
                  0 < t1 < t0 and x1 >= inside)

            def inner(at, k):
                tx, ty = g["tiles"][k]
                return tuple(at(tx + i, ty + j) for j in range(3, g["tile"] - 3)
                             for i in range(3, g["tile"] - 3))
            # (the program's tile hops for a moment after its click, so
            # the two are compared until the hop is over)
            same = False
            deadline = time.time() + 5
            while not same and time.time() < deadline:
                w, h, at = q.screen_raw()
                same = (inner(at, len(g["tiles"]) - 1) ==
                        inner(at, DOCK_N - 1))
                if not same:
                    time.sleep(0.3)
            check("and the last window's tile is where the layout says - "
                  "the same picture as its program's", same)

            third = read_clock_as(q, SHAPE)
            check("the clock can still be read with %d windows open "
                  "('%s')" % (DOCK_N, third), re.match(SHAPE, third) is not None)
            check("and it keeps running",
                  ok2 and re.match(SHAPE, third) is not None and
                  seconds(third) > seconds(second))

            # Regional formats change what the clock presents without
            # changing the underlying RTC value.
            q.click_desktop()
            time.sleep(0.5)
            for key, value in (("datefmt", "iso"), ("timefmt", "12h")):
                sudo_set(q, key, value)
            iso12 = (r"^[A-Z][a-z]{2} \d{4}-\d\d-\d\d "
                     r"\d{1,2}:\d\d:\d\d [AP]M$")
            regional = read_clock_as(q, iso12)
            check("ISO dates and a twelve-hour clock follow the Region "
                  "settings ('%s')" % regional,
                  re.match(iso12, regional) is not None)

            for key, value in (("datefmt", "dmy"), ("timefmt", "24h"),
                               ("language", "pl")):
                sudo_set(q, key, value)
            days = ["Pon", "Wt", "Śr", "Czw", "Pt", "Sob", "Ndz"]
            months = ["Sty", "Lut", "Mar", "Kwi", "Maj", "Cze",
                      "Lip", "Sie", "Wrz", "Paź", "Lis", "Gru"]
            expected = "%s %d %s" % (days[day.weekday()], day.day,
                                     months[day.month - 1])
            polish = read_clock_as(q, re.escape(expected) + " ")
            check("Polish language localizes the bar's date ('%s')" % polish,
                  polish.startswith(expected + " "))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
