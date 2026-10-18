#!/usr/bin/env python3
"""ThreadOS smoke test: the file manager.

What it replaced listed eight names in a window the size of a postage
stamp and showed the first hundred bytes of one of them. That proved
the path "disk -> click -> read -> drawing"; it was not a file
manager. A file manager is where a person GOES to find a file: it says
how big things are, it walks into directories and back out of them, it
hands a file to the program that opens it, and it throws one away when
asked twice.

The assertions follow exactly that. The sizes are checked against a
file whose length the test knows, because a column of numbers that
comes from nowhere proves nothing. Deleting is checked in both halves:
the first click only ASKS - and the file is still there afterwards -
and the second one does it, which the kernel's own shell then confirms
by failing to read the file. A folder is removed only when it is
empty, and after the same question.

The window's name is checked too: it says which directory it is
showing, and changes when we walk into one. Two file managers open on
two directories would be two windows called "Files" otherwise.

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

# The window: slot 2 of the cascade, content a pixel right and a title
# bar down; gfiles asks for GFILES_W x GFILES_H (user/gfiles.c).
WX, WY = 94, 54
CX, CY = WX + 1, WY + CONTENT_DY
CW, CH = GFILES_W, GFILES_H
# Its three buttons, kept at the right edge of the heading
BTN_Y = GFILES_HEAD_H // 2
DEL_W, EDIT_W, NAME_W = 70, 56, 70
DEL_X = CW - 10 - DEL_W
EDIT_X = DEL_X - 6 - EDIT_W
NAME_X = EDIT_X - 6 - NAME_W

# The question panel (user/ugui.h): a band across the middle of the
# window, with the two answers at its bottom edge
MODAL = ug_modal(CH)

TEXT = "Measurement results"          # what the test writes, and its length


def title(q):
    """The window's own name, as its title bar shows it."""
    return q.window_title(WX, WY)


def rows(q):
    """(name, size) per row - read in the lettering the program writes in"""
    return [(n, sz) for n, sz, _ in q.gfiles_rows(CX, CY)]


def row_y(names, name):
    return CY + gfiles_row_y(names.index(name))


def status(q):
    return q.gfiles_status(CX, CY)


def click(q, x, y, wait=1.5):
    q.mouse_goto(x, y)
    time.sleep(0.5)
    q.mouse_click()
    time.sleep(wait)


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")

    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()

            # Something to look at: a file whose length we know, and a
            # directory to walk into
            q.type_line("write report.txt " + TEXT)
            time.sleep(1)
            q.type_line("run mkdir documents")
            time.sleep(2)

            q.launch_dock(2)                   # the "Files" tile
            time.sleep(4)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()

            listed = rows(q)
            names = [r[0] for r in listed]
            check("the manager lists what is on the disk (%s)"
                  % ", ".join(names), "report.txt" in names)
            check("and the directory too (%s)" % ", ".join(names),
                  "documents" in names)

            # The size column against a file whose length we know: the
            # shell writes the text and a newline
            size = dict(listed).get("report.txt", "")
            check("a file has its size beside it ('%s' next to %d characters "
                  "of text)" % (size, len(TEXT)),
                  size.startswith(str(len(TEXT))) or
                  size.startswith(str(len(TEXT) + 1)))
            check("and a directory has a word instead of a number ('%s')"
                  % dict(listed).get("documents", ""),
                  dict(listed).get("documents", "") == "folder")

            head = q.gfiles_heading(CX, CY)
            check("the heading says where we are ('%s')" % head,
                  head.startswith("/"))
            check("and the window is named after the directory ('%s')" % title(q),
                  title(q) == "Files: /")

            # --- walking into a directory -------------------------------
            click(q, CX + GFILES_ROW_X, row_y(names, "documents"))
            click(q, CX + EDIT_X + EDIT_W // 2, CY + BTN_Y, wait=2.5)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            head = q.gfiles_heading(CX, CY)
            check("'Open' on a directory goes into it ('%s')" % head,
                  head.startswith("/documents"))
            check("and the window says the same ('%s')" % title(q),
                  title(q) == "Files: /documents")
            inside = [n for n, _ in rows(q)]
            check("and it is empty inside but for the way back up (%s)"
                  % ", ".join(inside), inside in ([], [".."]))

            # --- and back out -------------------------------------------
            click(q, CX + 250, CY + 16, wait=1)   # the focus into the window
            q.sendkey("backspace")
            time.sleep(2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("Backspace goes back up ('%s')" % title(q),
                  title(q) == "Files: /")
            listed = rows(q)
            names = [r[0] for r in listed]
            check("and what was there shows again", "report.txt" in names)

            # --- deleting asks first ------------------------------------
            click(q, CX + GFILES_ROW_X, row_y(names, "report.txt"))
            click(q, CX + DEL_X + DEL_W // 2, CY + BTN_Y, wait=2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            asked = q.ug_question(CX, CY, CH)
            check("a click on 'Remove' asks a question instead of deleting ('%s')"
                  % asked, "Remove" in asked and "report.txt" in asked)

            # The question removed nothing, and "Keep" has to leave it
            # alone - a question whose answers both mean yes is not a
            # question. (While it is up, the list behind it is dimmed.)
            click(q, CX + MODAL["no"][0], CY + MODAL["no"][1], wait=2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("'Keep' keeps the file",
                  "report.txt" in [r[0] for r in rows(q)])

            click(q, CX + DEL_X + DEL_W // 2, CY + BTN_Y, wait=2)
            click(q, CX + MODAL["yes"][0], CY + MODAL["yes"][1], wait=2.5)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("only 'Remove' in the question removes it",
                  "report.txt" not in [r[0] for r in rows(q)])

            # Confirmed by something that is not this program
            q.mouse_goto(500, 380)
            q.mouse_click()                    # the desktop takes the keys
            time.sleep(1)
            seen = len(q.serial())
            q.type_line("cat report.txt")
            time.sleep(2)
            after = q.serial()[seen:]
            check("and the file really is gone from the disk",
                  TEXT not in after)

            # --- a new name, asked for in the same status line ----------
            # The blocks do not move when a file is renamed, so this is
            # instant whatever its size - which is what the filesystem
            # grew a rename for.
            click(q, CX + 250, CY + 16, wait=1)
            names = [r[0] for r in rows(q)]
            click(q, CX + GFILES_ROW_X, row_y(names, "documents"))
            click(q, CX + NAME_X + NAME_W // 2, CY + BTN_Y, wait=2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            said = status(q)
            check("'Rename' asks for a new name ('%s')" % said,
                  "New name" in said)

            click(q, CX + 250, CY + 16, wait=1)   # the keys into the window
            q.type_line("papers", enter=False)
            time.sleep(0.5)
            q.sendkey("ret")
            time.sleep(2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            names = [r[0] for r in rows(q)]
            check("and it changes it (%s)" % ", ".join(names),
                  "papers" in names and "documents" not in names)

            # --- an empty folder is removed, after the same question ---
            # (one with something in it is refused before asking -
            # scripts/tests/t_filetree.py)
            click(q, CX + 250, CY + 16, wait=1)
            names = [r[0] for r in rows(q)]
            click(q, CX + GFILES_ROW_X, row_y(names, "papers"))
            click(q, CX + DEL_X + DEL_W // 2, CY + BTN_Y, wait=2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            asked = q.ug_question(CX, CY, CH)
            check("an empty folder is asked about too ('%s')" % asked,
                  "folder" in asked and "papers" in asked)
            click(q, CX + MODAL["yes"][0], CY + MODAL["yes"][1], wait=2.5)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("and 'Remove' removes it",
                  "papers" not in [r[0] for r in rows(q)])
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
