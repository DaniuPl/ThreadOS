#!/usr/bin/env python3
"""ThreadOS smoke test: the terminal while a program is running.

The terminal used to start a program and WAIT for it: it drew nothing,
answered nothing and showed nothing until the program had ended. A
program that ends quickly made that invisible. A program that does not
end took the window with it: no output, no way to close it, no way to
stop it. That is not a terminal running a program, that is a terminal
that has stopped.

Two things had to change, and they are what is checked here:

  The output is read AS IT IS WRITTEN. The program writes to a file and
  the terminal shows what is past the point it last read, on every pass
  of its own loop - so the lines appear while the program is still
  going, and go on appearing.

  Ctrl+C means the same thing it means everywhere. The key arrives as
  0x03 like any other, the terminal sends SIGINT to the program it
  started, and a program with no handler of its own dies on it - after
  which the terminal takes the next command.

The program is uupper64: it never ends by itself - it reads its input
until there is no more - and every line it is given comes back in
capitals. A line in capitals on the screen can only have come from the
program, written while it was still running, so each one is proof that
the output was read as it arrived and not when the program was over.

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


def wait_rows(q, ok, geom, tries=20):
    """Polls the window until what it shows passes the test 'ok'"""
    seen = []
    for _ in range(tries):
        seen = read_rows(q, **geom)
        if ok(seen):
            return seen
        time.sleep(0.5)
    return seen


def settled(q, ok, geom, tries=20):
    """Waits until what the window shows passes the test 'ok' in two
    pictures in a row. The terminal redraws its whole window on every
    pass and its 'running' notice last of all, so one picture taken
    halfway through can be missing it - and a notice that is gone has to
    be seen gone in finished pictures, not in a torn one."""
    seen, good = [], 0
    for _ in range(tries):
        seen = read_rows(q, **geom)
        good = good + 1 if ok(seen) else 0
        if good == 2:
            break
        time.sleep(0.5)
    return seen


def command_echo(rows, command):
    return any(row.startswith("/") and any(
        row.split(prompt, 1)[1] == command
        for prompt in ("> ", "# ") if prompt in row) for row in rows)


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

            q.launch_dock("gterm")                # the "Term" tile
            time.sleep(3)
            q.mouse_goto(CX + 200, CY + 150)
            q.mouse_click()                # focus
            time.sleep(1)
            q.mouse_goto(600, 380)
            time.sleep(1)

            q.type_line("uupper64")
            rows = wait_rows(q, lambda rs: "running" in "\n".join(rs), geom)
            check("the terminal says something is running and how to stop it",
                  "running" in "\n".join(rows) and "^C" in "\n".join(rows))

            # A line for it to answer: what comes back is the program's
            # output, and it has to be on the screen while the program
            # is still there
            q.type_line("alpha")
            rows = wait_rows(q, lambda rs: "ALPHA" in rs
                             and "running" in "\n".join(rs), geom)
            check("the output shows BEFORE the program finished (%s)"
                  % [r for r in rows if r][-3:],
                  "ALPHA" in rows and "running" in "\n".join(rows))

            # It goes on arriving - one read at the start, or one at the
            # end, would pass the check above and fail this one
            q.type_line("beta")
            rows = wait_rows(q, lambda rs: "BETA" in rs
                             and "running" in "\n".join(rs), geom)
            check("and more of it keeps coming (%s)"
                  % [r for r in rows if r][-5:],
                  "ALPHA" in rows and "BETA" in rows
                  and rows.index("ALPHA") < rows.index("BETA")
                  and "running" in "\n".join(rows))

            # The window still takes keys while the program runs: what
            # is typed lands on the line being edited
            # (polled: the terminal redraws its whole window on every
            # pass, and a picture taken in the middle of that can be
            # missing the line drawn last)
            for ch in "abc":
                q.sendkey(ch)
                time.sleep(0.2)
            rows = wait_rows(q, lambda rs: rs[-1].endswith("abc"), geom)
            check("the window takes keys while a program runs "
                  "('%s')" % rows[-1], rows[-1].endswith("abc"))
            for _ in range(3):
                q.sendkey("backspace")
                time.sleep(0.2)

            # --- Ctrl+C ------------------------------------------------
            q.sendkey("ctrl-c")
            rows = settled(q, lambda rs: "^C" in "\n".join(rs)
                           and "[code" in "\n".join(rs)
                           and "running" not in "\n".join(rs)
                           and rs[-1].startswith("/"), geom)
            body = "\n".join(rows)
            check("Ctrl+C was shown in the session", "^C" in body)
            check("the program stopped running (the running notice goes)",
                  "running" not in body)
            check("and the terminal gave its code", "[code" in body)

            # The notice going says only that the terminal stopped
            # watching. That the program is gone is for the system to
            # say: core's 'kill' looks it up by name among the programs
            # that are running, and is silent when it finds one (that it
            # does find a live one by name is t_consoles64's to check)
            q.type_line("kill uupper64")
            rows = wait_rows(q, lambda rs: any("no such process" in r
                                               for r in rs), geom)
            check("the program is really gone - there is no process left "
                  "to stop (%s)" % rows[-3:],
                  any("kill: no such process 'uupper64'" in r for r in rows))

            # --- and the terminal takes the next command ---------------
            q.type_line("echo after")
            rows = wait_rows(q, lambda rs: "after" in rs, geom)
            check("after the program is killed the terminal takes the next command",
                  command_echo(rows, "echo after"))
            check("and shows ITS output, not the leftovers of the last one",
                  "after" in rows)
            echo_at = max((i for i, r in enumerate(rows)
                           if command_echo([r], "echo after")), default=-1)
            check("nothing of the old program is left under the new command",
                  echo_at >= 0 and not any(r in ("ALPHA", "BETA")
                                           for r in rows[echo_at:]))

            # --- output with nothing typed -------------------------------
            # uupper64 only writes when it is given a line, so everything
            # above would pass a terminal that reads the program's output
            # only when a key is pressed. spin64 writes a numbered tick
            # every half second by itself: a later number turning up
            # with no key pressed in between can only come from the
            # terminal reading on its own.
            def ticks(rs):
                got = []
                for r in rs:
                    if r.startswith("[spin64] tick "):
                        tail = r[len("[spin64] tick "):]
                        if tail.isdigit():
                            got.append(int(tail))
                return got

            q.type_line("spin64")
            rows = wait_rows(q, lambda rs: ticks(rs) != [], geom, tries=40)
            first = max(ticks(rows), default=0)
            check("a program that writes by itself shows its first lines "
                  "(tick %d)" % first, first > 0)
            rows = wait_rows(q, lambda rs: max(ticks(rs), default=0)
                             >= first + 3, geom, tries=40)
            later = max(ticks(rows), default=0)
            check("and more of them arrive with no key pressed "
                  "(tick %d, then %d)" % (first, later), later >= first + 3)

            q.sendkey("ctrl-c")
            rows = settled(q, lambda rs: "running" not in "\n".join(rs)
                           and rs[-1].startswith("/"), geom)
            stopped = max(ticks(rows), default=0)
            time.sleep(2)              # four ticks' time, if it still ran
            rows = read_rows(q, **geom)
            check("Ctrl+C stops it: no tick arrives after the prompt is "
                  "back (%d, then %d)"
                  % (stopped, max(ticks(rows), default=0)),
                  stopped > 0 and max(ticks(rows), default=0) == stopped)
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
