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

  It does not get crowded out. With eleven windows open the dock's
  tiles get smaller to fit, and the clock - on the bar, in a place of
  its own - is still there and still running.

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


def read_clock(q):
    """The clock off the bar, in the bar's lettering"""
    x = bar_clock_x() + 4
    q.mouse_goto(x, 1)
    time.sleep(0.3)
    q.mouse_goto(x, BAR_H // 2)
    time.sleep(0.2)
    for px in range(BAR_PX, 7, -1):
        text = q.find_ui(bar_clock_x() - 90, SCREEN_W,
                         ui_font_baseline_in(BAR_PX, 0, BAR_H), charset=CHARS,
                         px=px, under=on_bar)
        if text:
            return text
    return ""


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
            q.mouse_goto(600, 300)
            time.sleep(1)

            first = read_clock(q)
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
            time.sleep(4)
            second = read_clock(q)
            check("four seconds later the clock shows something else ('%s')"
                  % second, second != first)
            ok2 = ok and re.match(SHAPE, second) is not None
            delta = seconds(second) - seconds(first) if ok2 else -1
            check("and it is a few seconds later, not just anything "
                  "(%d s)" % delta, 2 <= delta <= 8)

            # --- a crowded dock --------------------------------------
            for k in range(DOCK_N):
                q.launch_dock(k, n_w=1 + k)
                time.sleep(2.5)
            q.mouse_goto(600, 300)
            time.sleep(2)

            g = dock_geom(n_w=1 + DOCK_N)
            w, h, at = q.screen_raw()
            check("ten windows: the dock's tiles got smaller to fit "
                  "(%d px) and it ends inside the screen (at %d)"
                  % (g["tile"], g["x"] + g["w"]),
                  g["tile"] < DOCK_TILE_M and g["x"] + g["w"] <= w - DOCK_MARGIN)

            def inner(k):
                tx, ty = g["tiles"][k]
                return tuple(at(tx + i, ty + j) for j in range(3, g["tile"] - 3)
                             for i in range(3, g["tile"] - 3))
            check("and the last window's tile is where the layout says - "
                  "the same picture as its program's",
                  inner(len(g["tiles"]) - 1) == inner(DOCK_N - 1))

            third = read_clock(q)
            check("the clock can still be read with eleven windows open "
                  "('%s')" % third, re.match(SHAPE, third) is not None)
            check("and it keeps running",
                  ok2 and re.match(SHAPE, third) is not None and
                  seconds(third) > seconds(second))

            # Regional formats change what the clock presents without
            # changing the underlying RTC value.
            q.click_desktop()
            time.sleep(0.5)
            for key, value in (("datefmt", "iso"), ("timefmt", "12h")):
                q.type_line("sudo set %s %s" % (key, value))
                time.sleep(0.5)
                q.command("thread", "saved", timeout=30)
                time.sleep(1)
            regional = read_clock(q)
            check("ISO dates and a twelve-hour clock follow the Region "
                  "settings ('%s')" % regional,
                  re.match(r"^[A-Z][a-z]{2} \d{4}-\d\d-\d\d "
                           r"\d{1,2}:\d\d:\d\d [AP]M$", regional) is not None)

            for key, value in (("datefmt", "dmy"), ("timefmt", "24h"),
                               ("language", "pl")):
                q.type_line("sudo set %s %s" % (key, value))
                time.sleep(0.5)
                q.command("thread", "saved", timeout=30)
                time.sleep(1)
            polish = read_clock(q)
            days = ["Pon", "Wt", "Śr", "Czw", "Pt", "Sob", "Ndz"]
            months = ["Sty", "Lut", "Mar", "Kwi", "Maj", "Cze",
                      "Lip", "Sie", "Wrz", "Paź", "Lis", "Gru"]
            expected = "%s %d %s" % (days[day.weekday()], day.day,
                                     months[day.month - 1])
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
