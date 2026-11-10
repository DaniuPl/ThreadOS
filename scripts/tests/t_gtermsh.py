#!/usr/bin/env python3
"""ThreadOS smoke test: talking to a program from the terminal window.

The terminal could start a program and show what it printed. What it
could not do is say anything back to it - the program's input came from
nowhere, so anything that reads is anything the terminal could not run:
no shell, no program that asks a question, nothing interactive at all.

Now what is typed while a program runs goes down a pipe into that
program's stdin, and Ctrl+D closes it, which is how a reader is told
there will be no more. Two things had to be built for it:

  A way to START a program with a pipe into it. The terminal builds it
  itself out of pipe, fork and dup2 (user/uterm.h), and keeps the write
  end as an ordinary descriptor.

  A shared stdout. A shell's children have to write where the shell
  writes - and each descriptor carries an offset of its own, so the
  only place they can all agree on is the END of the file. Without that
  the shell's next prompt lands on top of what its child has just
  written, and the output disappears under it.

Which is why the proof here is the user shell, ush64, running INSIDE the
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
PROMPT = "ush64$"


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


def index_of(rows, want, after=-1):
    for i in range(after + 1, len(rows)):
        if want in rows[i]:
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
            q.type_line("ush64")
            rows = settled(q, lambda rs: PROMPT in "\n".join(rs)
                           and "running" in "\n".join(rs), geom)
            body = "\n".join(rows)
            check("the user's shell came up in the terminal window",
                  "the user's shell" in body)
            check("and it showed a prompt of its own", PROMPT in body)
            check("the terminal knows something is running in it", "running" in body)

            # --- what is typed reaches it -----------------------------
            # The terminal puts the line after the shell's prompt; the
            # shell, which echoes what it reads, writes it once more
            # under it - and that second copy is the proof it arrived.
            #
            # The order is the point: the command, then its output, then
            # the next prompt. Getting this wrong means the child's
            # output was written over by the shell. The prompt comes a
            # moment after the output, so all three are waited for.
            def order(rows):
                cmd_at = index_of(rows, PROMPT + " uecho64 hello")
                out_at = next((i for i in range(cmd_at + 1, len(rows))
                               if rows[i] == "hello"), -1) \
                    if cmd_at >= 0 else -1
                nxt = next((i for i in range(out_at + 1, len(rows))
                            if rows[i].startswith(PROMPT)), -1) \
                    if out_at >= 0 else -1
                return cmd_at, out_at, nxt

            q.type_line("uecho64 hello")
            rows = settled(q, lambda rs: order(rs)[2] >= 0, geom)
            cmd_at, out_at, nxt = order(rows)
            check("what was typed reached the shell (it echoed the command)",
                  cmd_at >= 0 and index_of(rows, "uecho64 hello", cmd_at) > cmd_at)
            check("the program's output is UNDER the command that called it",
                  cmd_at >= 0 and out_at > cmd_at)
            check("and the next prompt is under the output, not on it (%s)"
                  % [r for r in rows if r][-4:], out_at >= 0 and nxt > out_at)

            # --- and a real program with more output ------------------
            # hello64 is a child of the shell, writing to the same
            # stdout: three counted lines half a second apart, and then
            # the shell's word on how it ended (code 7)
            q.type_line("hello64")
            rows = wait_line(q, "ush64: done, code 7", geom)
            at = index_of(rows, PROMPT + " hello64")
            for n in (1, 2, 3):
                at = index_of(rows, "[hello64] counting: %d" % n, at) \
                    if at >= 0 else -1
            done = index_of(rows, "ush64: done, code 7", at) if at >= 0 else -1
            check("the lines hello64 writes show in order, and the shell's "
                  "report under them (%s)" % rows[-6:], at >= 0 and done > at)

            # A pipeline INSIDE the shell inside the terminal: core's wc
            # counts one line, three words and fourteen bytes
            q.type_line("uecho64 one two three | core wc")
            rows = settled(q, lambda rs: any(r.split() == ["1", "3", "14"]
                                             for r in rs), geom)
            check("a pipe inside the shell writes to the window too",
                  any(r.split() == ["1", "3", "14"] for r in rows))

            # --- Ctrl+D ends it ---------------------------------------
            q.sendkey("ctrl-d")
            rows = settled(q, lambda rs: "^D" in "\n".join(rs)
                           and "running" not in "\n".join(rs)
                           and rs[-1].startswith("/"), geom)
            body = "\n".join(rows)
            check("Ctrl+D shows in the session (%s)"
                  % [r for r in rows if r][-3:], "^D" in body)
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
