#!/usr/bin/env python3
"""ThreadOS smoke test: talking to a program from the terminal window.

The terminal could start a program and show what it printed. What it
could not do is say anything back to it - the program's input came from
nowhere, so anything that reads is anything the terminal could not run:
no shell, no program that asks a question, nothing interactive at all.

Now what is typed while a program runs goes down a pipe into that
program's stdin, and Ctrl+D closes it, which is how a reader is told
there will be no more. Two things had to be built for it:

  A way to START a program with a pipe into it. In protected mode the
  kernel does it (spawn keeps the write end and hands it back as an
  ordinary descriptor); in long mode the terminal builds it itself out
  of pipe, fork and dup2.

  A shared stdout. A shell's children have to write where the shell
  writes - and each descriptor carries an offset of its own, so the
  only place they can all agree on is the END of the file. Without that
  the shell's next prompt lands on top of what its child has just
  written, and the output disappears under it.

Which is why the proof here is the user shell running INSIDE the
terminal window: it reads what is typed, its children's output appears
under the command that made it and above the next prompt, and Ctrl+D
ends it.

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


def wait_line(q, want, geom, tries=20):
    seen = []
    for _ in range(tries):
        seen = read_rows(q, **geom)
        if any(want in r for r in seen):
            return seen
        time.sleep(0.5)
    return seen


def index_of(rows, want):
    for i, r in enumerate(rows):
        if want in r:
            return i
    return -1


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

            # --- a shell inside the terminal --------------------------
            q.type_line("sh")
            rows = wait_line(q, "ush:", geom)
            body = "\n".join(rows)
            check("the user's shell came up in the terminal window",
                  "the user's shell" in body)
            check("and it showed a prompt of its own", "ush:/>" in body)
            check("the terminal knows something is running in it", "running" in body)

            # --- what is typed reaches it -----------------------------
            q.type_line("echo hello")
            rows = wait_line(q, "hello", geom)
            check("what was typed reached the shell (it echoed the command)",
                  index_of(rows, "ush:/> echo hello") >= 0)

            # The order is the point: the command, then its output, then
            # the next prompt. Getting this wrong means the child's
            # output was written over by the shell.
            cmd_at = index_of(rows, "ush:/> echo hello")
            out_at = -1
            for i in range(cmd_at + 1, len(rows)):
                if rows[i] == "hello":
                    out_at = i
                    break
            check("the program's output is UNDER the command that called it",
                  out_at > cmd_at)
            nxt = -1
            for i in range(out_at + 1, len(rows)):
                if rows[i].startswith("ush:/>"):
                    nxt = i
                    break
            check("and the next prompt is under the output, not on it",
                  nxt > out_at)

            # --- and a real command with more output ------------------
            q.type_line("seq 3")
            rows = wait_line(q, "seq 3", geom)
            time.sleep(1.5)
            rows = read_rows(q, **geom)
            check("the numbers from 'seq 3' show in order",
                  "1" in rows and "2" in rows and "3" in rows)

            # A pipeline INSIDE the shell inside the terminal
            q.type_line("seq 9 | wc")
            rows = wait_line(q, "lines=9", geom)
            check("a pipe inside the shell writes to the window too",
                  "lines=9" in "\n".join(rows))

            # --- Ctrl+D ends it ---------------------------------------
            q.sendkey("ctrl-d")
            time.sleep(3)
            rows = read_rows(q, **geom)
            body = "\n".join(rows)
            check("Ctrl+D shows in the session", "^D" in body)
            check("the shell finished and the terminal stopped watching it",
                  "running" not in body)

            # --- the terminal is its own again ------------------------
            q.type_line("date")
            rows = wait_line(q, "2030-", geom)
            check("after the shell ends the terminal takes commands of its own",
                  "2030-06-13" in "\n".join(rows))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
