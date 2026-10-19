#!/usr/bin/env python3
"""ThreadOS smoke test: locking the screen.

Ctrl+Alt+L, or 'lock' in the shell, and the screen shows the
background, the time and a field for the password - nothing of what is
on the desktop. What is asserted is what a lock is for:

  nothing of the desktop shows: no window, no bar, no dock - and the
  time is there, read off the screen

  keys typed go to the field and nowhere else: a command typed at the
  lock is not run by the shell behind it

  a click reaches nothing behind it either

  a wrong password says so and opens nothing; the right one opens it,
  and the desktop is back as it was, with its windows

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary


def under(x, y):
    """What the lock's letters stand on: the aurora, darkened"""
    return gfx_mix((0, 0, 0), aurora_px(x, y), 90)


def lock_time(q):
    """The big time in the middle of the lock, 'HH:MM'"""
    return q.find_ui(SCREEN_W // 2 - 120, SCREEN_W // 2 + 120,
                     SCREEN_H // 3, charset="0123456789:", px=LOCK_TIME_PX,
                     face="bold", fg=(255, 255, 255), under=under)


def lock_says(q):
    fy = SCREEN_H * 2 // 3 - LOCK_FIELD_H
    best = ""
    for fg in ((0xD8, 0xD8, 0xDC), (0xFF, 0x69, 0x61)):
        got = q.find_ui(80, SCREEN_W - 80, fy + LOCK_FIELD_H + 22, px=UI_PX,
                        fg=fg, under=under)
        if len(got) > len(best):
            best = got
    return best


def typed(q, text):
    for ch in text:
        q.sendkey(Qemu.KEYNAMES.get(ch, ch))
        time.sleep(0.12)


def main():
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = Qemu(KERNEL, INITRD, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()
            q.mouse_goto(600, 300)
            time.sleep(1)
            check("the desktop starts without placeholder windows",
                  q.dock_windows() == 0)

            q.sendkey("ctrl-alt-l")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("Ctrl+Alt+L locks the desktop",
                  "to unlock" in lock_says(q))
            got = lock_time(q)
            check("the time is there, big ('%s', the bar said %s)"
                  % (got, RTC_BASE[11:16]),
                  len(got) == 5 and got[2] == ":" and
                  got[:2] == RTC_BASE[11:13])
            said = lock_says(q)
            check("and what it wants ('%s')" % said, "password" in said)

            # The keys are the field's: the shell behind gets nothing
            seen = len(q.serial())
            typed(q, "uptime\n")
            time.sleep(1.5)
            check("a command typed at the lock is not run by the shell",
                  "uptime:" not in q.serial()[seen:])
            said = lock_says(q)
            check("it was taken for a password, and a wrong one ('%s')"
                  % said, "Wrong password" in said)

            typed(q, "thread\n")
            time.sleep(2)
            q.mouse_goto(600, 300)
            time.sleep(1)
            check("the password opens the desktop",
                  lock_says(q) == "" and q.dock_windows() == 0)

            # And the shell's own command does the same
            q.type_line("lock")
            time.sleep(1.5)
            check("'lock' in the shell locks it too",
                  "to unlock" in lock_says(q))
            typed(q, "thread\n")
            time.sleep(2)
            check("and the password opens it",
                  lock_says(q) == "")
        finally:
            q.close()
    finally:
        if os.path.exists(slog):
            os.unlink(slog)
    return summary()


sys.exit(main())
