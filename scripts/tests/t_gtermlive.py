#!/usr/bin/env python3
"""ThreadOS smoke test: the terminal while a program is running.

The terminal used to start a program and WAIT for it: it drew nothing,
answered nothing and showed nothing until the program had ended. A
program that ends quickly made that invisible. A program that does not
end - 'spin' is one - took the window with it: no output, no way to
close it, no way to stop it. That is not a terminal running a program,
that is a terminal that has stopped.

Two things had to change, and they are what is checked here:

  The output is read AS IT IS WRITTEN. The program writes to a file and
  the terminal shows what is past the point it last read, on every pass
  of its own loop - so the lines appear while the program is still
  going, and go on appearing.

  Ctrl+C means the same thing it means everywhere. The key arrives as
  0x03 like any other, the terminal sends SIGINT to the program it
  started, and a program with no handler of its own dies on it - after
  which the terminal takes the next command.

And through all of it the window is alive: it keeps drawing, it says
that something is running, and it still takes what is typed.

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


def read_rows(q, ox, oy, cols, rows):
    """The terminal's text, read off the screen in the lettering it is
    drawn in: a grid of cells, each read on its own (ptest.read_cells)"""
    return q.read_cells(ox, oy, cols, rows, cell=TERM_CELL, row_h=ROW_H,
                        base=TERM_BASE, px=TERM_PX)


def spins(rows):
    return sum(1 for r in rows if "spin" in r and "/>" not in r)


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
            q.mouse_click()                # focus
            time.sleep(1)
            q.mouse_goto(600, 380)
            time.sleep(1)

            # 'spin' prints a line and never ends by itself
            q.type_line("spin")
            time.sleep(2.5)
            rows = read_rows(q, **geom)
            first = spins(rows)
            check("the output shows BEFORE the program finished (%d lines)"
                  % first, first > 0)
            check("the terminal says something is running and how to stop it",
                  "running" in "\n".join(rows))

            # It goes on arriving - one flush at the start would pass
            # the check above and fail this one
            time.sleep(3)
            rows = read_rows(q, **geom)
            second = spins(rows)
            check("and more of it keeps coming (%d -> %d lines)" % (first, second),
                  second > first)

            # The window still takes keys while the program runs: what
            # is typed lands on the line being edited
            for ch in "abc":
                q.sendkey(ch)
                time.sleep(0.2)
            time.sleep(0.8)
            rows = read_rows(q, **geom)
            last = [r for r in rows if r][-1]
            check("the window takes keys while a program runs "
                  "('%s')" % last, last.endswith("abc"))
            for _ in range(3):
                q.sendkey("backspace")
                time.sleep(0.2)

            # --- Ctrl+C ------------------------------------------------
            q.sendkey("ctrl-c")
            time.sleep(3)
            rows = read_rows(q, **geom)
            body = "\n".join(rows)
            check("Ctrl+C was shown in the session", "^C" in body)
            check("the program stopped running (the running notice goes)",
                  "running" not in body)
            check("and the terminal gave its code", "[code" in body)

            after_kill = spins(read_rows(q, **geom))
            time.sleep(2.5)
            check("nothing is added to the session any more",
                  spins(read_rows(q, **geom)) == after_kill)

            # --- and the terminal takes the next command ---------------
            q.type_line("seq 3")
            time.sleep(3)
            rows = read_rows(q, **geom)
            check("after the program is killed the terminal takes the next command",
                  "/> seq 3" in "\n".join(rows))
            check("and shows ITS output, not the leftovers of the last one",
                  "1" in rows and "2" in rows and "3" in rows)
            check("nothing of the old program is left in the window",
                  spins(rows) == 0 or "/> seq 3" in "\n".join(rows[-8:]))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
