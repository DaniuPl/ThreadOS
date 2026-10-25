#!/usr/bin/env python3
"""ThreadOS smoke test: the wheel - two fingers on a laptop's touchpad.

A touchpad on the PS/2 port is a mouse: it reports relative movement
and buttons exactly as one does, and its firmware turns a tap into a
click. What it has and a two-button mouse of 1987 did not is a wheel,
which is how it reports two fingers sliding. That fourth byte does not
come for free - it has to be asked for, with a knock nobody would send
by accident: three sample rates, 200, 100 and 80, after which the
device says who it is. Answer 3 means the wheel is there.

So there are two things to prove, and they are different things:

  that the asking worked - the machine says so, and QEMU's mouse only
  sends the fourth byte to a guest that asked
  that turning the wheel moves the thing UNDER THE POINTER, not the
  thing with the keyboard focus, and that turning it back brings the
  list back to where it started

The file manager is what is scrolled here, because a list of two dozen
names in a window that holds fewer is the plainest case there is: the
name at the top before and after is a number the test can read.

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

# The file manager: slot 2 of the cascade, content a pixel right and a
# title bar down (user/gfiles.c).
WX, WY = 94, 54
CX, CY = WX + 1, WY + CONTENT_DY

FILES = 24                      # more than the window shows


def first_row(q):
    """The name at the top of the list, read in the lettering the file
    manager writes it in"""
    rows = q.gfiles_rows(CX, CY)
    return rows[0][0] if rows else ""


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

            # --- the machine asked for the fourth byte ------------------
            # QEMU's PS/2 mouse sends it only to a guest that asked, so
            # this and the scrolling below are the two halves of one
            # question.
            seen = len(q.serial())
            q.type_line("mouse")
            time.sleep(2)
            said = q.serial()[seen:]
            check("the machine negotiated a wheel ('%s')"
                  % said.replace("\n", " ")[:60],
                  "wheel=yes" in said)

            # --- a list longer than the window -------------------------
            # Named so that the order is the order they were made in,
            # and so that a name read from the screen says which one it
            # is without counting.
            for i in range(FILES):
                q.type_line("write file%02d.txt x" % i)
                time.sleep(0.35)
            time.sleep(1)

            q.launch_dock("gfiles")                # the "Files" tile
            time.sleep(4)
            q.mouse_goto(500, 380)             # the pointer off the window
            time.sleep(1.5)

            # The disk has a swap file on it from boot, so what stands
            # at the top is whatever the directory begins with - the
            # test does not care which name it is, only that it is one
            # and that it moves.
            top_before = first_row(q)
            check("the file manager shows the list ('%s')" % top_before,
                  len(top_before) > 2)

            # --- the wheel over the list -------------------------------
            q.mouse_goto(CX + GFILES_LIST_X + 120, CY + 100)   # over the listing
            time.sleep(0.5)
            q.mouse_wheel(-2)                  # towards the person: down
            time.sleep(1)
            top_down = first_row(q)
            check("the wheel down scrolled the list ('%s' -> '%s')"
                  % (top_before, top_down),
                  top_down != top_before and top_down.startswith("file"))

            q.mouse_wheel(2)                   # and back up
            time.sleep(1)
            top_back = first_row(q)
            check("the wheel up came back where it was ('%s')" % top_back,
                  top_back == top_before)

            # --- and over nothing it does nothing ----------------------
            # The wheel goes to the window under the POINTER. With the
            # pointer on the desktop there is no such window, and the
            # list must not move just because it is the one in focus.
            q.mouse_goto(600, 300)
            time.sleep(0.5)
            q.mouse_wheel(-3)
            time.sleep(1)
            top_off = first_row(q)
            check("the wheel outside the window does not scroll it ('%s')" % top_off,
                  top_off == top_before)
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
