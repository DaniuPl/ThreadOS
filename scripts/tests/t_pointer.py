#!/usr/bin/env python3
"""ThreadOS test: the pointer's settings - its speed, and natural scrolling.

Pointer speed is a share of the hand's movement: at 200 % a movement of
fifty counts takes the pointer a hundred pixels, at 50 % twenty-five.
The pointer is pushed into the corner first, so where it ends is where
the movement put it, and the kernel's shell says where that is ('mouse').

Natural scrolling turns the wheel round: the content follows the
fingers. At the top of a list longer than its window, the wheel turned
away from the person does nothing; with natural scrolling it is what
moves the list down - and the other way brings it back.

Both are set from the kernel's shell ('sudo set pointer 200'), and the
desktop takes them up within a second.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64

# The file manager in the cascade's second place (user/gfiles.c)
WX, WY = 94, 54
CX, CY = WX + 1, WY + CONTENT_DY
FILES = 24


def first_row(q):
    rows = q.gfiles_rows(CX, CY)
    return rows[0][0] if rows else ""


def set_as_admin(q, line):
    """Typed to the kernel's shell, which has the keys while no window
    is open - no click, since a click needs the pointer where the test
    thinks it is, and the speed is what is being changed"""
    q.type_line("sudo " + line)
    time.sleep(1)
    q.command("thread", "saved", timeout=30)
    time.sleep(1.5)               # the desktop reads settings once a second


def where(q):
    out = q.command("mouse", "wheel=", timeout=15)
    m = re.search(r"mouse: x=(\d+) y=(\d+)", out)
    return (int(m.group(1)), int(m.group(2))) if m else None


def main():
    print()
    print("--- the pointer's speed and natural scrolling ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    try:
        check("the disc with the 64-bit kernel builds", make_iso64(d, iso))
        q = Qemu(KERNEL, INITRD, disk=disk, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=60)
            q.type_line("desktop")
            check("the desktop came up", q.wait_desktop())
            q.click_desktop()

            q.mouse_home()
            q.mouse_move(50, 40)
            got = where(q)
            check("at the usual speed, a movement of (50, 40) puts the "
                  "pointer at (50, 40): %s" % (got,), got == (50, 40))

            for pct, want in ((200, (100, 80)), (50, (25, 20))):
                set_as_admin(q, "set pointer %d" % pct)
                q.mouse_home()
                q.mouse_move(50, 40)
                got = where(q)
                check("at %d %%, the same movement puts it at %s: %s"
                      % (pct, want, got), got == want)

            set_as_admin(q, "set pointer 100")
            q.mouse_home()

            # --- natural scrolling, on a list longer than its window -----
            # Files opens at the root; the shell starts in the home folder
            q.type_line("cd /")
            time.sleep(0.5)
            for i in range(FILES):
                q.type_line("write file%02d.txt x" % i)
                time.sleep(0.35)
            time.sleep(1)
            q.launch_dock("gfiles")
            time.sleep(4)
            q.mouse_goto(CX + GFILES_LIST_X + 120, CY + 100)
            time.sleep(1.5)
            top = first_row(q)
            check("the file manager shows the list ('%s')" % top,
                  len(top) > 2)

            q.mouse_wheel(2)               # away from the person
            time.sleep(1)
            check("without natural scrolling, the wheel up at the top of "
                  "the list does nothing ('%s')" % first_row(q),
                  first_row(q) == top)

            q.click_desktop()
            set_as_admin(q, "set natural 1")
            q.mouse_goto(CX + GFILES_LIST_X + 120, CY + 100)
            time.sleep(0.5)
            q.mouse_wheel(2)
            time.sleep(1)
            down = first_row(q)
            check("with it, the same wheel moves the list down ('%s' -> "
                  "'%s')" % (top, down),
                  down != top and down.startswith("file"))
            q.mouse_wheel(-2)
            time.sleep(1)
            check("and the other way brings it back ('%s')" % first_row(q),
                  first_row(q) == top)
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, disk):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
