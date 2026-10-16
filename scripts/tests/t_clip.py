#!/usr/bin/env python3
"""ThreadOS smoke test: the clipboard.

Copying and pasting is the plainest thing two programs do together, and
the one thing they cannot do by themselves: neither may touch the
other's memory and neither knows the other exists. So the text has to
wait somewhere that belongs to both - which means the kernel.

Here it is 512 bytes of text and two calls, reached by the two oldest
keys for it: Ctrl+Insert takes the line being typed, Shift+Insert puts
it back. Ctrl+C is not used for it, because in a terminal Ctrl+C means
'stop that program' and nothing else.

The assertions are about the thing that makes a clipboard a clipboard -
that the text crosses from one program to another:

  from a window to another window: typed in the terminal, pasted into
  the settings window, where it is a value being edited
  from a window back to the console: the same text pasted into a shell
  that never saw it typed
  and the kernel keeps it: the window it was copied from can be closed
  and the text is still there, because it was never that window's

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
CX, CY = WX + 1, WY + 11
PAD, ROW_H = 4, 10
COLS, ROWS = (400 - 8) // 8, (240 - 8) // 10
WORD = "moved"


def read_rows(q, ox, oy, cols, rows, pitch=ROW_H, per_row=False):
    """The text of a window, read through the kernel's own font.

    'per_row' computes the background line by line instead of once for
    the block: the settings window paints the SELECTED row a different
    colour, and a row read against somebody else's background comes out
    as nothing but question marks."""
    w, h, at = q.screen_raw()
    font = font8x8()

    def background(rows_range):
        counts = {}
        for r in rows_range:
            for y in range(8):
                for x in range(ox, ox + cols * 8, 2):
                    p = at(x, oy + r * pitch + y)
                    counts[p] = counts.get(p, 0) + 1
        return max(counts, key=counts.get)

    bg = None if per_row else background(range(rows))

    out = []
    for r in range(rows):
        if per_row:
            bg = background([r])
        line = ""
        for c in range(cols):
            pattern = []
            for y in range(8):
                bits = 0
                for x in range(8):
                    if at(ox + c * 8 + x, oy + r * pitch + y) != bg:
                        bits |= 1 << x
                pattern.append(bits)
            line += font.get(tuple(pattern), " " if not any(pattern) else "?")
        out.append(line.rstrip())
    return out


def typed(q, text):
    for ch in text:
        q.sendkey(q.KEYNAMES.get(ch, ch))
        time.sleep(0.12)


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    geom = dict(ox=CX + PAD, oy=CY + PAD, cols=COLS, rows=ROWS)

    print("--- the clipboard between windows ---")
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()

            # --- copy: in the terminal ---------------------------------
            q.mouse_goto(24, 192)          # the 'Term' icon
            q.mouse_click()
            time.sleep(3)
            q.mouse_goto(CX + 200, CY + 150)
            q.mouse_click()
            time.sleep(1)
            q.mouse_goto(600, 380)
            time.sleep(1)

            typed(q, WORD)
            time.sleep(0.5)
            rows = read_rows(q, **geom)
            check("the text is on the terminal's line",
                  any(WORD in r for r in rows))
            q.sendkey("ctrl-insert")       # copy
            time.sleep(0.5)

            # Clear the line, so that what appears later cannot be what
            # was left lying there
            for _ in range(len(WORD)):
                q.sendkey("backspace")
                time.sleep(0.08)
            time.sleep(0.5)
            rows = read_rows(q, **geom)
            check("the terminal's line is empty again",
                  not any(WORD in r for r in rows))

            # --- paste: in another window ------------------------------
            q.mouse_goto(24, 272)          # the 'Config' icon
            q.mouse_click()
            time.sleep(3)
            q.mouse_goto(600, 380)
            time.sleep(1.5)

            # Slot 3: content at (107,77); the 'name' row is the first,
            # at ROW_Y=22, and its value is drawn at x=100
            SX, SY = 107, 77
            q.mouse_goto(SX + 40, SY + 22)
            q.mouse_click()                # select the row
            time.sleep(1)
            q.sendkey("shift-insert")      # paste
            time.sleep(1.5)
            q.mouse_goto(600, 380)
            time.sleep(1.5)

            # The value of a setting is drawn at x=100 of the content,
            # so it has a grid of its own - eight pixels wide from
            # THERE, not from the left edge of the window
            vals = read_rows(q, ox=SX + 100, oy=SY + 22, cols=30, rows=4,
                             pitch=14, per_row=True)
            check("the text from the terminal appeared in another window ('%s')"
                  % vals[0], WORD in vals[0])

            # --- and the kernel keeps it -------------------------------
            # Close the window it came from: the text was never that
            # window's, so closing it changes nothing
            q.mouse_goto(WX + 402 - 6, WY + 5)     # the terminal's [x]
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(3)

            # Paste again, into the settings window's next row
            q.mouse_goto(SX + 40, SY + 22 + 14)
            q.mouse_click()
            time.sleep(1)
            q.sendkey("shift-insert")
            time.sleep(1.5)
            q.mouse_goto(600, 380)
            time.sleep(1.5)
            vals = read_rows(q, ox=SX + 100, oy=SY + 22, cols=30, rows=4,
                             pitch=14, per_row=True)
            check("the clipboard survived the closing of the window it came from "
                  "('%s')" % vals[1], WORD in vals[1])
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    # --- and the same clipboard on the text console -------------------
    print("--- the same clipboard on the console ---")
    slog2 = tempfile.mktemp(suffix=".log")
    try:
        q2 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog2)
        try:
            q2.wait_for("ThreadOS shell", timeout=40)
            typed(q2, "echo " + WORD)
            q2.sendkey("ctrl-insert")      # copy the whole line
            time.sleep(0.5)
            for _ in range(len("echo " + WORD)):
                q2.sendkey("backspace")
                time.sleep(0.05)

            # Another console: a different shell, a different line
            # buffer - and the same clipboard, because it is the
            # kernel's
            q2.sendkey("alt-f2")
            time.sleep(1.5)
            q2.sendkey("shift-insert")
            time.sleep(1)
            screen = q2.screen()
            check("the pasted line appeared on the SECOND console",
                  "echo " + WORD in screen)
            q2.sendkey("ret")
            out = q2.wait_for(WORD, timeout=20)
            check("and it ran as if it had been typed",
                  WORD in out)
        finally:
            q2.close()
    finally:
        if os.path.exists(slog2):
            os.unlink(slog2)

    return summary()


sys.exit(main())
