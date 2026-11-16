#!/usr/bin/env python3
"""ThreadOS smoke test: locking the screen.

Ctrl+Alt+L, or 'lock' in the shell, and the screen shows the
background, the time and a field for the password - nothing of what is
on the desktop. What is asserted is what a lock is for:

  nothing of the desktop shows: no window, no bar, no dock - and the
  time is there, read off the screen

  keys typed go to the field and nowhere else: a command typed at the
  lock is not run by the shell behind it

  a click reaches nothing behind it either: not the close light of the
  window under it, not a program's tile in the dock

  a wrong password says so and opens nothing; the right one opens it,
  and the desktop is back as it was, with its window

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

# The pointer rests here, off the terminal and off the lock's lettering
REST = (600, 300)


def under_for(w, h):
    """What the lock's letters stand on: the aurora, darkened - the
    aurora of the screen's own size, which GRUB makes 800x600"""
    return lambda x, y: gfx_mix((0, 0, 0), aurora_px(x, y, w, h), 90)


def lock_time(q):
    """The big time in the middle of the lock, 'HH:MM'"""
    w, h = q.size()
    return q.find_ui(w // 2 - 120, w // 2 + 120, h // 3,
                     charset="0123456789:", px=LOCK_TIME_PX, face="bold",
                     fg=(255, 255, 255), under=under_for(w, h))


def lock_says(q):
    w, h = q.size()
    fy = h * 2 // 3 - LOCK_FIELD_H
    best = ""
    for fg in ((0xD8, 0xD8, 0xDC), (0xFF, 0x69, 0x61)):
        got = q.find_ui(80, w - 80, fy + LOCK_FIELD_H + 22, px=UI_PX,
                        fg=fg, under=under_for(w, h))
        if len(got) > len(best):
            best = got
    return best


def wait_says(q, text, timeout=15):
    """What the lock says, read until it says 'text' or the time is up"""
    said = ""
    end = time.time() + timeout
    while time.time() < end:
        said = lock_says(q)
        if text in said:
            break
        time.sleep(0.3)
    return said


def desktop_showing(at):
    """The first place that is not the darkened aurora, on a grid over
    the whole screen - None when there is none. The lock draws only its
    lettering and its field, in a band across the middle (kernel/
    desktop.c, draw_lock()), and the pointer: those are left out, and
    everything else is where the bar, the dock, the Desktop's icons and
    the terminal stood."""
    top = SCREEN_H // 3 - LOCK_TIME_PX - 8
    bottom = SCREEN_H * 2 // 3 + 32
    for y in range(2, SCREEN_H, 8):
        for x in range(2, SCREEN_W, 8):
            if (SCREEN_W // 2 - 170 <= x <= SCREEN_W // 2 + 170 and
                    top <= y <= bottom):
                continue
            if (REST[0] - 8 <= x <= REST[0] + 32 and
                    REST[1] - 8 <= y <= REST[1] + 40):
                continue
            if not near(at(x, y), under(x, y), 2):
                return (x, y), at(x, y), under(x, y)
    return None


def typed(q, text):
    for ch in text:
        q.sendkey(Qemu.KEYNAMES.get(ch, ch))
        time.sleep(0.12)


def terminal_title(q, timeout=20):
    """The first window's title, read until it is the terminal's"""
    got = ""
    end = time.time() + timeout
    while time.time() < end:
        got = q.window_title(94, 54)
        if got.startswith("Terminal"):
            break
        time.sleep(0.3)
    return got


def windows_seen(q, hold=8):
    """Every count of windows the dock shows over 'hold' seconds. A
    program takes seconds to start, so a click that did reach a tile
    shows as a second window only after a while - one look would miss
    it."""
    seen = set()
    end = time.time() + hold
    while time.time() < end:
        seen.add(q.dock_windows())
        time.sleep(0.5)
    return sorted(seen)


def main():
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = Qemu(KERNEL, INITRD, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop())
            q.mouse_home()
            q.mouse_goto(*REST)
            time.sleep(1)
            check("the desktop starts without placeholder windows",
                  q.dock_windows() == 0)

            # A window for the lock to hide and to give back. A click on
            # the bare desktop then hands the keys to the kernel's shell,
            # whose serial output is how a key that got past the lock
            # would be seen.
            q.launch_dock("gterm")
            q.mouse_goto(*REST)
            title = terminal_title(q)
            check("a terminal is open under the lock-to-be ('%s')" % title,
                  title.startswith("Terminal"))
            q.click_desktop()
            q.mouse_goto(*REST)
            check("a click on the bare desktop takes the focus from the "
                  "terminal", q.wait_focus(94, 54, want=False, timeout=10))

            q.sendkey("ctrl-alt-l")
            said = wait_says(q, "to unlock")
            check("Ctrl+Alt+L locks the desktop ('%s')" % said,
                  "to unlock" in said)
            _, _, at = q.screen_raw()
            shown = desktop_showing(at)
            check("nothing of the desktop shows - no window, no bar, no "
                  "dock (%s)" % (shown or "the darkened aurora all over",),
                  shown is None)
            got = lock_time(q)
            check("the time is there, big ('%s', the bar said %s)"
                  % (got, RTC_BASE[11:16]),
                  len(got) == 5 and got[2] == ":" and
                  got[:2] == RTC_BASE[11:13])
            said = lock_says(q)
            check("and what it wants ('%s')" % said, "password" in said)

            # Clicks where the terminal's close light and the Files tile
            # in the dock are. Had either got through, the desktop given
            # back below would have no terminal or, sooner or later, a
            # second window - which is why the dock is watched there for
            # a while, and looked at once more at the very end.
            q.click(*light_xy(94, 54, 0))
            q.click(*dock_tile(dock_index("gfiles"), n_w=1))
            q.mouse_goto(*REST)
            said = wait_says(q, "to unlock", timeout=5)
            check("a click at the lock leaves it locked ('%s')" % said,
                  "to unlock" in said)

            # The keys are the field's: the shell behind gets nothing.
            # The lock saying the password was wrong is the sign that
            # the Enter has been taken - by then the shell, had it been
            # given the line, would have answered it too.
            seen = len(q.serial())
            typed(q, "uptime\n")
            said = wait_says(q, "Wrong password", timeout=10)
            time.sleep(0.5)
            check("a command typed at the lock is not run by the shell",
                  "Uptime:" not in q.serial()[seen:])
            check("it was taken for a password, and a wrong one ('%s')"
                  % said, "Wrong password" in said)

            # The lock draws no window, so a title that can be read is
            # the lock gone (and the hint's place is the terminal's
            # inside now, not a line to read as empty)
            typed(q, "thread\n")
            q.mouse_goto(*REST)
            title = terminal_title(q)
            check("the password opens the desktop ('%s')" % title,
                  title.startswith("Terminal"))
            counts = windows_seen(q)
            check("the desktop is back as it was: the terminal ('%s') and "
                  "no other window, then or later (in the dock: %s)"
                  % (title, counts),
                  title.startswith("Terminal") and counts == [1])

            # Unlocked, the same command does reach the shell - which is
            # what makes its absence above mean something
            out = q.command("uptime", "Uptime:", timeout=15)
            check("unlocked, the same command is run by the shell",
                  "Uptime:" in out)

            # And the shell's own command does the same
            q.type_line("lock")
            said = wait_says(q, "to unlock")
            check("'lock' in the shell locks it too",
                  "to unlock" in said)
            typed(q, "thread\n")
            title = terminal_title(q)
            check("and the password opens it ('%s')" % title,
                  title.startswith("Terminal"))
            n = q.dock_windows()
            check("with the one window it had all along (%d in the dock)"
                  % n, n == 1)
        finally:
            q.close()
    finally:
        if os.path.exists(slog):
            os.unlink(slog)
    return summary()


sys.exit(main())
