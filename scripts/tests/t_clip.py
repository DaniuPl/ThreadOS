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
from ptest import check, summary

WX, WY = 94, 54
CX, CY = WX + 1, WY + CONTENT_DY
PAD, ROW_H = TERM_PAD, TERM_ROW_H
COLS, ROWS = TERM_COLS, TERM_ROWS
WORD = "moved"


def read_rows(q, ox, oy, cols, rows, per_row=False):
    """The terminal's text, read off the screen in the lettering it is
    drawn in: a grid of cells, each read on its own (ptest.read_cells)"""
    return q.read_cells(ox, oy, cols, rows, cell=TERM_CELL, row_h=ROW_H,
                        base=TERM_BASE, px=TERM_PX)


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
            q.launch_dock(4)                # the "Term" tile
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
            q.launch_dock(6)                # the "Settings" tile
            time.sleep(3)
            q.mouse_goto(600, 380)
            time.sleep(1.5)

            # Slot 1 of the cascade; it opens on General, whose first
            # setting is the machine's name, in a field
            sx, sy = cascade(3, GSET_W + 2, GSET_H + TITLE_H + 2)
            SX, SY = sx + 1, sy + CONTENT_DY
            name_field = gset_field(0)[0]
            q.click(SX + name_field[0], SY + name_field[1])   # the keys
            time.sleep(1)
            q.sendkey("shift-insert")      # paste
            time.sleep(1.5)
            q.mouse_goto(600, 380)
            time.sleep(1.5)

            val = q.gset_field_text(SX, SY, 0)
            check("the text from the terminal appeared in another window ('%s')"
                  % val, WORD in val)

            # --- and the kernel keeps it -------------------------------
            # Close the window it came from: the text was never that
            # window's, so closing it changes nothing
            q.mouse_goto(*light_xy(WX, WY, 0))   # the terminal's red light
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(3)

            # Paste again, into another field: Display's resolution
            q.click(SX + gset_section("Display")[0],
                    SY + gset_section("Display")[1])
            time.sleep(1)
            res_field = gset_field(1)[0]
            q.click(SX + res_field[0], SY + res_field[1])
            time.sleep(1)
            q.sendkey("shift-insert")
            time.sleep(1.5)
            q.mouse_goto(600, 380)
            time.sleep(1.5)
            val = q.gset_field_text(SX, SY, 1)
            check("the clipboard survived the closing of the window it came from "
                  "('%s')" % val, WORD in val)
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
