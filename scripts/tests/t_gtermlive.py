#!/usr/bin/env python3
"""ProtonOS smoke test: the terminal while a program is running.

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
from ptest import check, summary, font8x8

WX, WY = 94, 54
CX, CY = WX + 1, WY + 11
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
            check("pulpit wstal", q.wait_desktop(timeout=40))
            q.mouse_home()

            q.mouse_goto(24, 192)          # the 'Term' icon
            q.mouse_click()
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
            check("wyjscie widac, ZANIM program sie skonczyl (%d linii)"
                  % first, first > 0)
            check("terminal mowi, ze cos chodzi, i jak to zatrzymac",
                  "running" in "\n".join(rows))

            # It goes on arriving - one flush at the start would pass
            # the check above and fail this one
            time.sleep(3)
            rows = read_rows(q, **geom)
            second = spins(rows)
            check("i przybywa go dalej (%d -> %d linii)" % (first, second),
                  second > first)

            # The window still takes keys while the program runs: what
            # is typed lands on the line being edited
            for ch in "abc":
                q.sendkey(ch)
                time.sleep(0.2)
            time.sleep(0.8)
            rows = read_rows(q, **geom)
            last = [r for r in rows if r][-1]
            check("okno przyjmuje klawisze przy dzialajacym programie "
                  "('%s')" % last, last.endswith("abc"))
            for _ in range(3):
                q.sendkey("backspace")
                time.sleep(0.2)

            # --- Ctrl+C ------------------------------------------------
            q.sendkey("ctrl-c")
            time.sleep(3)
            rows = read_rows(q, **geom)
            body = "\n".join(rows)
            check("Ctrl+C zostal pokazany w sesji", "^C" in body)
            check("program przestal chodzic (znika napis o dzialaniu)",
                  "running" not in body)
            check("i terminal podal jego kod", "[code" in body)

            after_kill = spins(read_rows(q, **geom))
            time.sleep(2.5)
            check("nic juz nie dopisuje sie do sesji",
                  spins(read_rows(q, **geom)) == after_kill)

            # --- and the terminal takes the next command ---------------
            q.type_line("seq 3")
            time.sleep(3)
            rows = read_rows(q, **geom)
            check("po zabiciu programu terminal bierze nastepna komende",
                  "/> seq 3" in "\n".join(rows))
            check("i pokazuje JEJ wyjscie, a nie resztek poprzedniego",
                  "1" in rows and "2" in rows and "3" in rows)
            check("po starym programie nie zostalo nic w oknie",
                  spins(rows) == 0 or "/> seq 3" in "\n".join(rows[-8:]))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
