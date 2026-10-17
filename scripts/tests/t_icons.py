#!/usr/bin/env python3
"""ThreadOS smoke test: the programs in the dock come from a file.

Nine programs, decided when the kernel was built. Adding one meant
editing kernel/desktop.c and building a kernel - which is the same as
saying that a person cannot add one.

The list is now a file on the disk, one line per icon:

    program  caption  numer-obrazka

and nothing else. Plain text like everything else the system keeps: it
can be read with 'cat' and BUILT ON THE MACHINE ITSELF, with the shell
in the terminal and the redirections it already had -

    echo gterm Terminal 4 > desktop.icons
    echo gweb Internet 5 >> desktop.icons

which is what this test does, because a format nobody on the machine
can produce is a format for somebody else's machine.

What is checked: the file is really read (the kernel says so and the
dock has as many programs as the file has lines, with the names and
pictures the file gave them), the tiles still WORK (clicking one starts
its program), and a machine whose file says nothing usable keeps the
nine it was born with - a dock with nothing in it is a machine with
nothing to click, and no broken line should be able to do that.

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

def picture_of(at, g, k):
    """The inside of the k-th tile in the dock"""
    tx, ty = g["tiles"][k]
    t = g["tile"]
    return tuple(at(tx + i, ty + j) for j in range(3, t - 3)
                 for i in range(3, t - 3))


def write_list(q, lines):
    """Builds the file with the ring 3 shell - '>' for the first line
    and '>>' for the rest, exactly as a person would."""
    q.type_line("run sh")
    time.sleep(2)
    for i, line in enumerate(lines):
        q.type_line("echo %s %s %d" % (line[0], line[1], line[2]) +
                    (" > " if i == 0 else " >> ") + "desktop.icons")
        time.sleep(1.5)
    q.type_line("exit")
    time.sleep(1.5)


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    wanted = [("gterm", "Terminal", 4),
              ("gweb", "Internet", 5),
              ("gsettings", "Settings", 6)]

    try:
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
        try:
            q.wait_for("ThreadOS shell", timeout=40)
            write_list(q, wanted)

            shown = q.command("cat desktop.icons", "gsettings", timeout=30)
            for app, label, art in wanted:
                check("the file has a line for '%s'" % app,
                      "%s %s %d" % (app, label, art) in shown)

            q.type_line("desktop")
            time.sleep(6)
            q.mouse_home()
            q.mouse_goto(560, 300)
            time.sleep(2)

            log = q.serial()
            check("the kernel read the icon list and says how many",
                  "icons from the file 'desktop.icons': 3" in log)

            # Three programs and the kernel's two windows: the dock is
            # laid out for exactly that - a dot under each window's
            # tile where three programs put it, and not where four or
            # nine would have
            check("there are three programs in the dock, and not one more",
                  q.dock_windows(n_l=3) == 2 and q.dock_windows(n_l=4) != 2
                  and q.dock_windows() != 2)

            for k, (app, label, art) in enumerate(wanted):
                got = q.dock_hint(k, n_l=3)
                check("tile %d carries the name from the file ('%s')"
                      % (k, got), got == label)
            q.mouse_goto(560, 300)
            time.sleep(1.5)

            g = dock_geom(n_l=3)
            w, h, at = q.screen_raw()
            pics = [picture_of(at, g, k) for k in range(3)]
            check("and each has a different picture",
                  pics[0] != pics[1] and pics[1] != pics[2] and
                  pics[0] != pics[2])

            # The tiles still do what they are for
            q.click(*dock_tile(0, n_l=3))
            time.sleep(4)
            log = q.serial()
            check("a click on a tile from the file starts its program",
                  "exec 'gterm'" in log)
        finally:
            q.close()

        # --- a list that says nothing usable ---------------------------
        print("--- a file with no sensible lines ---")
        slog2 = tempfile.mktemp(suffix=".log")
        q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog2)
        try:
            q2.wait_for("ThreadOS shell", timeout=40)
            # A file with nothing in it but a comment: the lines that
            # do not parse are passed over, and what is left is nothing
            q2.command("write desktop.icons #only-a-comment", "wrote",
                       timeout=30)
            q2.type_line("desktop")
            time.sleep(6)
            q2.mouse_home()
            q2.mouse_goto(560, 300)
            time.sleep(2)

            check("the dock comes back to its nine programs",
                  q2.dock_windows() == 2)
            first = q2.dock_hint(0)
            check("and the first of them is the usual one ('%s')" % first,
                  first == "App")
        finally:
            q2.close()
            if os.path.exists(slog2):
                os.unlink(slog2)
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
