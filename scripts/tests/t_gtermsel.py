#!/usr/bin/env python3
"""ThreadOS smoke test: selecting text with the mouse.

The clipboard could take the line being TYPED. What it could not take
was anything a program had written - which is most of what is in a
terminal, and the part somebody actually wants to copy: an address, a
number, the name of a file that came out of a listing.

Selecting needs something the compositor did not report. A click says
WHERE something happened; a selection is what happened BETWEEN two
places, so the desktop now sends three events for a drag inside a
window's content - the button going down, the pointer moving with it
held, the button coming up - in the content's own coordinates. What
they mean is the application's business; the compositor knows nothing
about selections.

The terminal turns them into a range in its ring of lines, not on the
screen: what is selected stays selected while the view scrolls under
it. And letting the button go COPIES, the way it has worked in
terminals for forty years - selecting is copying, there is no second
key to press.

The assertions are pixels and text: the selected part is drawn on a
field of its own, and what comes back out of the clipboard is exactly
what was under the mouse.

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

WX, WY = 94, 54
CX, CY = WX + 1, WY + CONTENT_DY
PAD, ROW_H = 4, 10
COLS, ROWS = (400 - 8) // 8, (240 - 8) // 10


def read_rows(q, ox, oy, cols, rows):
    w, h, at = q.screen_raw()
    font = font8x8()
    counts = {}
    for r in range(rows):
        for y in range(8):
            for x in range(ox, ox + cols * 8, 2):
                p = at(x, oy + r * ROW_H + y)
                counts[p] = counts.get(p, 0) + 1
    bg = max(counts, key=counts.get)

    out = []
    for r in range(rows):
        line = ""
        for c in range(cols):
            pattern = []
            for y in range(8):
                bits = 0
                for x in range(8):
                    if at(ox + c * 8 + x, oy + r * ROW_H + y) != bg:
                        bits |= 1 << x
                pattern.append(bits)
            line += font.get(tuple(pattern), " " if not any(pattern) else "?")
        out.append(line.rstrip())
    return out


def blue_at(at, x, y):
    r, g, b = at(x, y)
    return b > 120 and r < 90 and g < 90


def drag(q, x0, y0, x1, y1):
    q.mouse_goto(x0, y0)
    q.mouse_button(True)
    time.sleep(0.3)
    q.mouse_goto(x1, y1)
    time.sleep(0.4)
    q.mouse_goto(x1, y1)
    time.sleep(0.2)
    q.mouse_button(False)
    time.sleep(0.8)


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    geom = dict(ox=CX + PAD, oy=CY + PAD, cols=COLS, rows=ROWS)

    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()

            q.launch_dock(4)                # the "Term" tile
            time.sleep(3)
            q.mouse_goto(CX + 200, CY + 150)
            q.mouse_click()
            time.sleep(1)

            # Something written by a PROGRAM, not typed: the date. It
            # lands on the fourth line of the session (0: the greeting
            # wrapped over two, 2: the echoed command, 3: the output).
            q.type_line("date")
            time.sleep(3)
            rows = read_rows(q, **geom)
            line_no = next((i for i, r in enumerate(rows)
                            if r.startswith("2030-")), -1)
            check("the program's output is in the window", line_no >= 0)
            if line_no < 0:
                return summary()
            wanted = rows[line_no][:10]     # '2030-06-13'

            # --- the drag ------------------------------------------------
            y = CY + PAD + line_no * ROW_H + 4
            drag(q, CX + PAD, y, CX + PAD + 10 * 8, y)
            q.mouse_goto(600, 380)
            time.sleep(1)

            w, h, at = q.screen_raw()
            # The selected part is drawn on a field of its own - between
            # the glyphs there is now blue where there was black. The
            # field is eight pixels tall from the row's top, so it is
            # looked for on the rows the letters themselves are on.
            field = sum(1 for dx in range(0, 10 * 8)
                        for dy in (0, 2)
                        if blue_at(at, CX + PAD + dx, y + dy))
            check("the selected piece has a field under it (%d pixels)" % field,
                  field > 20)
            check("and what was not selected has no field",
                  not any(blue_at(at, CX + PAD + 12 * 8 + dx, y + dy)
                          for dx in range(0, 8) for dy in (0, 2)))

            # --- and letting go copied it --------------------------------
            q.sendkey("shift-insert")
            time.sleep(1.5)
            rows = read_rows(q, **geom)
            typed_line = [r for r in rows if r][-1]
            check("what is pasted is exactly what was under the mouse "
                  "('%s' w '%s')" % (wanted, typed_line),
                  wanted in typed_line)

            # Clear the line again
            for _ in range(len(wanted) + 2):
                q.sendkey("backspace")
                time.sleep(0.06)

            # --- two lines at once ---------------------------------------
            # A selection is a range in the ring of lines, so it can
            # cross them; the newline between is dropped when it lands in
            # a line being typed, but both parts are there
            y2 = CY + PAD + (line_no - 1) * ROW_H + 4
            drag(q, CX + PAD, y2, CX + PAD + 6 * 8, y)
            q.mouse_goto(600, 380)
            time.sleep(1)
            q.sendkey("shift-insert")
            time.sleep(1.5)
            rows = read_rows(q, **geom)
            typed_line = [r for r in rows if r][-1]
            check("a selection across two lines takes both ('%s')"
                  % typed_line,
                  "date" in typed_line and "2030-" in typed_line)

            for _ in range(30):
                q.sendkey("backspace")
                time.sleep(0.04)

            # --- a click is not a selection ------------------------------
            q.mouse_goto(CX + PAD + 4 * 8, y)
            q.mouse_click()
            time.sleep(1)
            q.mouse_goto(600, 380)
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("a click on its own clears the selection",
                  not any(blue_at(at, CX + PAD + dx, y + dy)
                          for dx in range(0, 10 * 8) for dy in (0, 2)))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
