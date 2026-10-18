#!/usr/bin/env python3
"""ThreadOS smoke test: asking for the password in the graphical part.

The kernel refuses a plain user who tries to change the system. On a
console that is the end of the story - the person types 'sudo' and the
shell asks. In a window there is no shell to ask, and an application in
ring 3 cannot raise its own rights: it can only be told 'not allowed'
and do something about it.

What it does about it is ask, and that is what this group is for. Both
of the machine's windows that need it ask in their own window, with
the keyboard they already have:

  the terminal, on 'sudo <command>': the line turns into a password
  line, what is typed shows as stars, and the command runs once - after
  which the terminal is a plain user again

  the settings window, when the write comes back refused: a panel over
  it asks for the password, and the same write is tried again

Both go through the same syscall, and a wrong password gets the same
answer in both: nothing happens and it says so.

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

# The terminal opens in slot 2: content at (95,65), padded by 4, rows
# 10 apart, 400x240 of content.
WX, WY = 94, 54
CX, CY = WX + 1, WY + CONTENT_DY
PAD, ROW_H = TERM_PAD, TERM_ROW_H
COLS, ROWS = TERM_COLS, TERM_ROWS


def read_rows(q, ox, oy, cols, rows):
    """The terminal's text, read off the screen in the lettering it is
    drawn in: a grid of cells, each read on its own (ptest.read_cells)"""
    return q.read_cells(ox, oy, cols, rows, cell=TERM_CELL, row_h=ROW_H,
                        base=TERM_BASE, px=TERM_PX)


def wait_row(q, want, geom, tries=15):
    seen = []
    for _ in range(tries):
        seen = read_rows(q, **geom)
        if any(want in r for r in seen):
            return seen
        time.sleep(0.5)
    return seen


def window_says(q, cx, cy, want, tries=8):
    """Does the line at the bottom of the settings window say that? It
    is redrawn when the program gets round to it, so we look again
    rather than assume."""
    said = ""
    for _ in range(tries):
        said = q.gset_status(cx, cy)
        if want in said:
            return True
        time.sleep(0.6)
    print("      (the window says '%s')" % said)
    return False


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

    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()

            # --- the terminal ------------------------------------------
            q.launch_dock(4)                # the "Term" tile
            time.sleep(3)
            q.mouse_goto(CX + 200, CY + 150)
            q.mouse_click()
            time.sleep(1)
            q.mouse_goto(600, 380)
            time.sleep(1)

            q.type_line("whoami")
            rows = wait_row(q, "plain user", geom)
            check("the terminal comes up as an ordinary user",
                  "plain user" in "\n".join(rows))

            # A program in ring 3 asking to delete a system file is
            # refused by the KERNEL - and rm says which refusal it was
            q.type_line("rm swap.sys")
            rows = wait_row(q, "permission", geom)
            check("the program is refused, not told 'there is no file'",
                  "no permission" in "\n".join(rows))

            # --- sudo in the window ------------------------------------
            q.type_line("sudo cp termout system.cfg")
            time.sleep(1.5)
            rows = read_rows(q, **geom)
            check("the terminal asks for a password",
                  any(r.startswith("password:") for r in rows))

            # A wrong one changes nothing and says so
            typed(q, "wrongpassword")
            q.sendkey("ret")
            rows = wait_row(q, "wrong password", geom)
            check("a wrong password is refused", "wrong password" in "\n".join(rows))

            q.type_line("sudo cp termout system.cfg")
            time.sleep(1.5)
            # What is typed shows as stars: the line has as many of them
            # as there were keys, and none of the letters
            typed(q, "thread")
            time.sleep(0.5)
            rows = read_rows(q, **geom)
            last = [r for r in rows if r][-1]
            check("the password being typed shows as stars ('%s')" % last,
                  "******" in last and "thread" not in last)

            q.sendkey("ret")
            time.sleep(3)
            rows = wait_row(q, "sudo cp", geom)
            body = "\n".join(rows)
            check("the command ran with admin rights",
                  "no permission" not in body.split("sudo cp")[-1])

            q.type_line("whoami")
            time.sleep(2)
            rows = read_rows(q, **geom)
            # Twice: once at the start of the session and once now,
            # after the one command that was run as an admin
            check("and after it the terminal is an ordinary user again",
                  "\n".join(rows).count("plain user") >= 2)

            # --- the settings window ------------------------------------
            q.launch_dock(6)                # the "Settings" tile
            time.sleep(3)
            q.mouse_goto(600, 380)
            time.sleep(1.5)

            # Slot 3 of the cascade - which on this screen is back at
            # the start. A background asked for under Appearance.
            sx, sy = cascade(3, GSET_W + 2, GSET_H + TITLE_H + 2)
            SX, SY = sx + 1, sy + CONTENT_DY
            q.click(SX + gset_section("Appearance")[0],
                    SY + gset_section("Appearance")[1])
            time.sleep(1)
            green = gset_swatch(3)
            q.click(SX + green[0], SY + green[1])
            time.sleep(2)
            q.mouse_goto(600, 380)
            time.sleep(1.5)

            asked = q.gset_asks(SX, SY)
            check("the settings window says admin is needed ('%s')" % asked,
                  asked == "Changing settings needs admin")

            typed(q, "wrongpassword")
            q.sendkey("ret")
            time.sleep(2)
            check("a wrong password in the window writes nothing either",
                  window_says(q, SX, SY, "NOT changed (wrong password)"))

            q.click(SX + green[0], SY + green[1])     # ask again
            time.sleep(1.5)
            typed(q, "thread")
            q.sendkey("ret")
            time.sleep(2.5)
            q.mouse_goto(600, 380)
            time.sleep(1.5)
            check("the right password saves the setting",
                  window_says(q, SX, SY, "Saved"))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
