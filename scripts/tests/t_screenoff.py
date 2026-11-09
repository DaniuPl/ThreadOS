#!/usr/bin/env python3
"""ThreadOS test: the screen going dark when nobody is there.

'screenoff' seconds with no key, no movement and no click, and the
desktop goes black; for the last part of the wait it is dimmed first,
as a warning. Anything brings it back - and what brought it back goes
no further: a click on a dark screen must not start the program under
it, because nobody could see what was there. With 'lockoff' the lock
is up when the screen comes back.

Everything is read off the screen, against the aurora the test computes
itself: dimmed is darker than the aurora and not black, dark is black
everywhere, back is the aurora again.

Run on its own or through scripts/run_tests.py.
"""

import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64

# On the bare desktop, clear of the icons down the left, the bar and
# the dock
SPOTS = ((600, 300), (560, 200), (450, 350), (380, 160))
# Where the pointer waits: off every spot, since the pointer is drawn
# over the dimming
AWAY = (630, 250)


def shade(q):
    """'aurora', 'dim', 'black' or 'other', from four spots of desktop"""
    w, h, at = q.screen_raw()
    got = [at(x, y) for x, y in SPOTS]
    want = [aurora_px(x, y, w, h) for x, y in SPOTS]
    if all(max(g) <= 4 for g in got):
        return "black"
    if all(all(abs(g[i] - a[i]) <= 25 for i in range(3))
           for g, a in zip(got, want)):
        return "aurora"
    # Dimmed: every spot darker than the aurora by a good margin, and not
    # black - about half as bright
    if all(0 < sum(g) < sum(a) * 0.75 for g, a in zip(got, want)):
        return "dim"
    return "other"


def wait_shade(q, want, timeout):
    seen = ""
    end = time.time() + timeout
    while time.time() < end:
        seen = shade(q)
        if seen == want:
            return seen
        time.sleep(0.25)
    return seen


def as_admin(q, line):
    q.click_desktop()
    q.type_line("sudo " + line)
    time.sleep(1)
    q.command("thread", "saved", timeout=30)


def lock_says(q):
    w, h, at = q.screen_raw()
    fy = h * 2 // 3 - LOCK_FIELD_H
    under = lambda x, y: gfx_mix((0, 0, 0), aurora_px(x, y, w, h), 90)
    return q.find_ui(80, w - 80, fy + LOCK_FIELD_H + 22, px=UI_PX,
                     fg=(0xD8, 0xD8, 0xDC), under=under, screen=(w, h, at))


def main():
    print()
    print("--- the screen going dark ---")
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
            check("the desktop came up", q.wait_desktop(40))
            q.mouse_home()
            q.mouse_goto(*AWAY)
            time.sleep(1)
            check("the desktop shows the aurora", shade(q) == "aurora")

            # Twelve seconds: dimmed from six, dark at twelve
            as_admin(q, "set screenoff 12")
            q.mouse_goto(*AWAY)
            t0 = time.time()
            got = wait_shade(q, "dim", 12)
            t_dim = time.time() - t0
            check("dimmed before it goes dark (%s after %.1f s)"
                  % (got, t_dim), got == "dim" and 3 < t_dim < 11)
            got = wait_shade(q, "black", 12)
            t_dark = time.time() - t0
            check("and then dark, everywhere (%s after %.1f s)"
                  % (got, t_dark), got == "black" and t_dark > t_dim)

            # A movement brings it back
            q.mouse_move(3, 0)
            q.mx += 3
            got = wait_shade(q, "aurora", 4)
            check("a movement brings the screen back (%s)" % got,
                  got == "aurora")

            # A click on a dark screen wakes it and opens nothing - the
            # pointer on the Files tile, where a click would open Files
            tx, ty = dock_tile(dock_index("gfiles"), n_w=0)
            q.mouse_goto(tx, ty)
            got = wait_shade(q, "black", 16)
            check("dark again with the pointer on a tile (%s)" % got,
                  got == "black")
            q.mouse_click()
            got = wait_shade(q, "dim", 4)
            if got != "aurora":
                got = wait_shade(q, "aurora", 4)
            time.sleep(2)
            check("the click woke the screen and opened nothing (%d windows)"
                  % q.dock_windows(), got == "aurora" and q.dock_windows() == 0)
            # The same click on a lit screen does open it
            q.mouse_click()
            time.sleep(3)
            check("the same click on a lit screen opens Files (%d windows)"
                  % q.dock_windows(), q.dock_windows() == 1)

            # The password when it comes back
            q.mouse_goto(*AWAY)
            as_admin(q, "set lockoff 1")
            q.mouse_goto(*AWAY)
            got = wait_shade(q, "black", 16)
            check("with 'lockoff' it goes dark too (%s)" % got,
                  got == "black")
            q.mouse_move(3, 0)
            q.mx += 3
            said = ""
            end = time.time() + 5
            while time.time() < end and "unlock" not in said:
                said = lock_says(q)
                time.sleep(0.3)
            check("and comes back locked ('%s')" % said, "unlock" in said)
            q.type_line("thread")
            time.sleep(2)
            q.mouse_goto(*AWAY)
            time.sleep(1)
            check("the password opens it, Files still open (%d windows)"
                  % q.dock_windows(),
                  lock_says(q) == "" and q.dock_windows() == 1)

            # Never: nothing happens however long nobody is there
            as_admin(q, "set screenoff 0")
            q.mouse_goto(*AWAY)
            time.sleep(16)
            check("with 'screenoff 0' it stays lit (%s)" % shade(q),
                  shade(q) != "black")
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
