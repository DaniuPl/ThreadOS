#!/usr/bin/env python3
"""ThreadOS smoke test: Alt+Tab.

With twelve windows to hand, the only way to reach one of them was the
mouse: a click on the window, or on its tile in the dock. Every
system a person has used switches windows from the keyboard, and the
keys for it were already there - the driver knew Alt was held down and
it knew Tab. Nothing put the two together.

Now Alt+Tab hands the focus to the next window and brings it to the
top. The order is the order of the slots, which is the order of the
windows' tiles in the dock - so pressing it twice takes you where
looking at the dock says it will. Minimised windows are skipped, because a focus
you cannot see is a keyboard typing into nowhere. Only application
windows participate in the switch, so every focus has somewhere to
send keys.

The assertions are about the two things that make it worth having: the
window that takes the focus comes to the TOP (its title bar whole, not
half covered by the one that was there), and the KEYS FOLLOW - what is
typed after the switch lands in the window that was switched to. That
last one is checked by typing a command into the terminal after
Alt+Tab has picked it out from under two other windows.

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

# The cascade: slot i sits at (70+12i, 30+12i).
def slot_xy(i):
    return 70 + 12 * i, 30 + 12 * i


def focused_tile(at, n_w):
    """Which window's tile in the dock has the accent dot under it - the
    window with the focus. -1 when none has."""
    g = dock_geom(n_w=n_w)
    acc = theme("dark")["accent"]
    for j in range(n_w):
        tx, ty = g["tiles"][DOCK_N + j]
        if near(at(tx + g["tile"] // 2, ty + g["tile"] + 1 + DOCK_DOT // 2),
                acc, 50):
            return j
    return -1


def title_whole(at, x, y, w):
    """Is that window's title bar visible along its whole width? A
    window under another one has its bar cut off where the other covers
    it. Read along the bar's top, above what is written on it."""
    bg = theme("dark")["title_bg"]
    return all(near(at(x + dx, y + 4), bg, 4) for dx in range(70, w - 16, 8))


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

            # Three applications, in this order: the terminal, the clock,
            # then settings.
            q.launch_dock("gterm", n_w=0)
            time.sleep(3)
            q.launch_menu("gclock")
            time.sleep(3)
            q.launch_dock("gsettings", n_w=2)
            time.sleep(3)
            q.mouse_goto(600, 400)
            time.sleep(2)

            w, h, at = q.screen_raw()
            start = focused_tile(at, 3)
            check("the window opened last has the focus (its tile in the "
                  "dock, %d, has the accent dot)" % start, start == 2)

            # --- Alt+Tab ------------------------------------------------
            # Slot order wraps: from slot 2 the next application is slot 0.
            q.sendkey("alt-tab")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            now = focused_tile(at, 3)
            check("Alt+Tab moved the focus to the next window (%d -> %d)"
                  % (start, now), now == 0)

            # It is on top, not merely focused: its title bar is whole
            x2, y2 = slot_xy(2)
            check("and the window came to the top - its whole title bar is visible",
                  title_whole(at, x2, y2, 402))

            # --- the keys follow the focus ------------------------------
            # Slot 0 is the terminal. If the focus is really there, what
            # we type lands in it - and it says so on its own screen.
            q.type_line("date")
            time.sleep(3)
            w, h, at = q.screen_raw()

            # The terminal's lines, read in the lettering they are drawn in
            text = q.term_rows(x2 + 1, y2 + CONTENT_DY, screen=(w, h, at))
            body = "\n".join(text)
            check("what was typed after Alt+Tab reached the terminal",
                  "/> date" in body)
            check("and the terminal ran it", "2030-06-13" in body)

            # --- round the wheel ----------------------------------------
            for _ in range(3):
                q.sendkey("alt-tab")
                time.sleep(1)
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("three Alt+Tabs with three windows return to the same one",
                  focused_tile(at, 3) == 0)

            # --- a minimised window is skipped --------------------------
            # Slot 0's yellow light minimises it; that must take it out
            # of the ring, not leave a focus nobody can see
            q.mouse_goto(*light_xy(x2, y2, 1))
            time.sleep(0.5)
            q.mouse_click()
            q.mouse_goto(600, 400)
            time.sleep(1.5)
            q.sendkey("alt-tab")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            after = focused_tile(at, 3)
            check("once a window is minimised Alt+Tab skips it (the focus on %d)"
                  % after, after in (1, 2))

            check("the desktop is still running",
                  at(600, 300)[0] + at(600, 300)[1] + at(600, 300)[2] > 20)
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
