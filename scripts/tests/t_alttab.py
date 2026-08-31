#!/usr/bin/env python3
"""ProtonOS smoke test: Alt+Tab.

With twelve windows to hand, the only way to reach one of them was the
mouse: a click on the window, or on its button on the taskbar. Every
system a person has used switches windows from the keyboard, and the
keys for it were already there - the driver knew Alt was held down and
it knew Tab. Nothing put the two together.

Now Alt+Tab hands the focus to the next window and brings it to the
top. The order is the order of the slots, which is the order of the
buttons on the taskbar - so pressing it twice takes you where looking
at the bar says it will. Minimised windows are skipped, because a focus
you cannot see is a keyboard typing into nowhere, and so are the
kernel's own windows, which have no application behind them to take a
key.

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
from ptest import check, summary, font8x8

TB_H = 12
TBL_X0 = 46
CLOCK_W = 8 * 8 + 6

# The cascade: slot i sits at (70+12i, 30+12i). Slots 0 and 1 are the
# kernel's own windows, so applications start at slot 2.
def slot_xy(i):
    return 70 + 12 * i, 30 + 12 * i


def buttons(at, w, y):
    out, start = [], None
    for x in range(TBL_X0, w - CLOCK_W - 2):
        r, g, b = at(x, y)
        on_bar = r > 160 and g > 160 and b > 160
        if not on_bar and start is None:
            start = x
        elif on_bar and start is not None:
            out.append((start, x - 1))
            start = None
    if start is not None:
        out.append((start, w - CLOCK_W - 3))
    return out


def focused_button(at, y, spans):
    """Which of the taskbar buttons is the blue (focused) one."""
    for i, (a, b) in enumerate(spans):
        r, g, bl = at((a + b) // 2, y)
        if bl > 120 and r < 90 and g < 90:
            return i
    return -1


def title_whole(at, x, y, w):
    """Is that window's title bar visible along its whole width? A
    window under another one has its bar cut off where the other covers
    it.

    We look between the title text (which is white, drawn at the left
    end) and the three buttons (at the right), so that what is measured
    is the bar itself and not what is written on it."""
    return all(at(x + dx, y + 5)[2] > 120 and at(x + dx, y + 5)[0] < 90
               for dx in range(100, w - 40, 8))


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

            # Three applications, in this order: the terminal (slot 2),
            # the clock (slot 3), the settings (slot 4)
            for iy in (192, 72, 272):
                q.mouse_goto(24, iy)
                q.mouse_click()
                time.sleep(3)
            q.mouse_goto(600, 400)
            time.sleep(2)

            w, h, at = q.screen_raw()
            row = h - TB_H + 10
            spans = buttons(at, w, row)
            check("trzy aplikacje maja swoje przyciski (razem %d z oknami "
                  "jadra)" % len(spans), len(spans) == 5)
            start = focused_button(at, row, spans)
            check("ostatnio otwarte okno ma ognisko (przycisk %d)" % start,
                  start == 4)

            # --- Alt+Tab ------------------------------------------------
            # Slot order wraps: from slot 4 the next application is the
            # one in slot 2, because slots 0 and 1 are the kernel's
            q.sendkey("alt-tab")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            spans = buttons(at, w, row)
            now = focused_button(at, row, spans)
            check("Alt+Tab przeniosl ognisko na nastepne okno (%d -> %d)"
                  % (start, now), now == 2)

            # It is on top, not merely focused: its title bar is whole
            x2, y2 = slot_xy(2)
            check("i okno wyszlo na wierzch - caly jego pasek tytulu widac",
                  title_whole(at, x2, y2, 402))

            # --- the keys follow the focus ------------------------------
            # Slot 2 is the terminal. If the focus is really there, what
            # we type lands in it - and it says so on its own screen.
            q.type_line("date")
            time.sleep(3)
            w, h, at = q.screen_raw()

            font = font8x8()
            # Read the terminal's lines: content at (x+1, y+11), padded
            # by 4, rows 10 apart
            ox, oy = x2 + 1 + 4, y2 + 11 + 4
            counts = {}
            for r in range(23):
                for yy in range(8):
                    for xx in range(ox, ox + 48 * 8, 2):
                        p = at(xx, oy + r * 10 + yy)
                        counts[p] = counts.get(p, 0) + 1
            bg = max(counts, key=counts.get)
            text = []
            for r in range(23):
                line = ""
                for c in range(48):
                    pattern = []
                    for yy in range(8):
                        bits = 0
                        for xx in range(8):
                            if at(ox + c * 8 + xx, oy + r * 10 + yy) != bg:
                                bits |= 1 << xx
                        pattern.append(bits)
                    line += font.get(tuple(pattern),
                                     " " if not any(pattern) else "?")
                text.append(line.rstrip())
            body = "\n".join(text)
            check("to, co wpisano po Alt+Tab, trafilo do terminala",
                  "/> date" in body)
            check("i terminal to wykonal", "2030-06-13" in body)

            # --- round the wheel ----------------------------------------
            for _ in range(3):
                q.sendkey("alt-tab")
                time.sleep(1)
            time.sleep(1)
            w, h, at = q.screen_raw()
            spans = buttons(at, w, row)
            check("trzy razy Alt+Tab przy trzech oknach wraca tam, gdzie "
                  "bylismy", focused_button(at, row, spans) == 2)

            # --- a minimised window is skipped --------------------------
            # Slot 2's [_] is at x+w-2*8-2; minimising it must take it
            # out of the ring, not leave a focus nobody can see
            q.mouse_goto(x2 + 402 - 2 * 8 - 2 + 4, y2 + 5)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(1.5)
            q.sendkey("alt-tab")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            spans = buttons(at, w, row)
            after = focused_button(at, row, spans)
            check("po zwinieciu okna Alt+Tab je pomija (ognisko na %d)"
                  % after, after in (3, 4))

            check("pulpit dziala dalej",
                  at(600, 300)[0] + at(600, 300)[1] + at(600, 300)[2] > 20)
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
