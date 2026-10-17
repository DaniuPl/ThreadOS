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
by failing to read the file. A directory is not deleted at all.

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
from ptest import check, summary, font8x8

# The window: slot 2 of the cascade, content a pixel right and a title
# bar down; gfiles asks for 320x220.
WX, WY = 94, 54
CX, CY = WX + 1, WY + CONTENT_DY
CW, CH = 320, 220
ROW_Y0 = 18
ROW_H = 10
DEL_X = CW - 48
EDIT_X = CW - 48 - 56 - 4
NAME_X = CW - 48 - 56 - 56 - 8

# The question panel (user/ugui.h): a band across the middle of the
# window, with the two answers at its bottom edge.
MODAL_H = 96
MODAL_Y = (CH - MODAL_H) // 2
MODAL_BY = MODAL_Y + MODAL_H - 26
YES_X = 8 + 8 + 42            # the middle of "Remove"
NO_X = 8 + 8 + 84 + 10 + 42   # the middle of "Keep"

TEXT = "Measurement results"          # what the test writes, and its length


def read_at(at, x, y, cols):
    """Text inside the window, read the way a person reads it: whatever
    colour most of the band is, is the background - the rest is ink.

    Counting the background rather than naming it is what makes one
    function read both kinds of row. An ordinary row is dark text on
    the window's grey; the chosen row is white text on the accent - and
    the accent is also the colour a directory's NAME is written in, so
    a rule that said 'blue is background' would read those names as
    empty."""
    font = font8x8()
    band = [at(CX + x + c * 8 + xx, CY + y + yy)
            for c in range(cols) for xx in range(8) for yy in range(8)]
    bg = max(set(band), key=band.count)
    out = ""
    for c in range(cols):
        pattern = []
        for yy in range(8):
            bits = 0
            for xx in range(8):
                if at(CX + x + c * 8 + xx, CY + y + yy) != bg:
                    bits |= 1 << xx
            pattern.append(bits)
        out += font.get(tuple(pattern), " " if not any(pattern) else "?")
    return out.strip()


def title(q):
    """The window's own name, as its title bar shows it."""
    return q.window_title(WX, WY)


def rows(at, n=8):
    out = []
    for i in range(n):
        name = read_at(at, 6, ROW_Y0 + i * ROW_H, 14)
        if not name:
            break
        # The size is drawn right-aligned, its left edge depending on
        # how many digits it has - so the grid is anchored to the RIGHT
        # edge, which does not move: the last glyph ends six pixels
        # short of the window's edge.
        out.append((name, read_at(at, CW - 6 - 12 * 8,
                                  ROW_Y0 + i * ROW_H, 12)))
    return out


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

            listed = rows(at)
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

            check("the heading says where we are ('%s')"
                  % read_at(at, 4, 2, 20), read_at(at, 4, 2, 20).startswith("/"))
            check("and the window is named after the directory ('%s')" % title(q),
                  title(q) == "Files: /")

            # --- walking into a directory -------------------------------
            i = names.index("documents")
            click(q, CX + 60, CY + ROW_Y0 + i * ROW_H + 3)
            click(q, CX + EDIT_X + 20, CY + 4, wait=2.5)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("'Open' on a directory goes into it ('%s')"
                  % read_at(at, 4, 2, 20),
                  read_at(at, 4, 2, 20).startswith("/documents"))
            check("and the window says the same ('%s')" % title(q),
                  title(q) == "Files: /documents")
            check("and it is empty inside", rows(at) == [])

            # --- and back out -------------------------------------------
            click(q, CX + 160, CY + 120, wait=1)   # the focus into the window
            q.sendkey("backspace")
            time.sleep(2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("Backspace goes back up ('%s')" % title(q),
                  title(q) == "Files: /")
            listed = rows(at)
            names = [r[0] for r in listed]
            check("and what was there shows again", "report.txt" in names)

            # --- deleting asks first ------------------------------------
            i = names.index("report.txt")
            click(q, CX + 60, CY + ROW_Y0 + i * ROW_H + 3)
            click(q, CX + DEL_X + 16, CY + 4, wait=2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            asked = read_at(at, 8 + 8, MODAL_Y + 10, 36)
            check("a click on 'Remove' asks a question instead of deleting ('%s')"
                  % asked, "Remove" in asked and "report.txt" in asked)
            check("and the file is still there",
                  "report.txt" in [r[0] for r in rows(at)])

            # "Keep" has to leave it alone - a question whose answers
            # both mean yes is not a question.
            click(q, CX + NO_X, CY + MODAL_BY + 8, wait=2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("'Keep' keeps the file",
                  "report.txt" in [r[0] for r in rows(at)])

            click(q, CX + DEL_X + 16, CY + 4, wait=2)
            click(q, CX + YES_X, CY + MODAL_BY + 8, wait=2.5)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("only 'Remove' in the question removes it",
                  "report.txt" not in [r[0] for r in rows(at)])

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
            click(q, CX + 160, CY + 120, wait=1)
            w, h, at = q.screen_raw()
            names = [r[0] for r in rows(at)]
            i = names.index("documents")
            click(q, CX + 60, CY + ROW_Y0 + i * ROW_H + 3)
            click(q, CX + NAME_X + 20, CY + 4, wait=2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("'Rename' asks for a new name ('%s')"
                  % read_at(at, 4, CH - 10, 30),
                  "New name" in read_at(at, 4, CH - 10, 30))

            click(q, CX + 160, CY + 120, wait=1)   # the keys into the window
            q.type_line("papers", enter=False)
            time.sleep(0.5)
            q.sendkey("ret")
            time.sleep(2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            names = [r[0] for r in rows(at)]
            check("and it changes it (%s)" % ", ".join(names),
                  "papers" in names and "documents" not in names)

            # --- a directory is not deleted -----------------------------
            click(q, CX + 160, CY + 120, wait=1)
            w, h, at = q.screen_raw()
            names = [r[0] for r in rows(at)]
            i = names.index("papers")
            click(q, CX + 60, CY + ROW_Y0 + i * ROW_H + 3)
            click(q, CX + DEL_X + 16, CY + 4, wait=2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            said = read_at(at, 4, CH - 10, 36)
            check("it does not remove a directory, and says why ('%s')" % said,
                  "directory" in said)
            check("and the directory is still there",
                  "papers" in [r[0] for r in rows(at)])
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
