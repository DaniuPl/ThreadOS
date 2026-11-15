#!/usr/bin/env python3
"""ThreadOS smoke test: a wallpaper behind the desktop.

The background was a colour, and then sixteen bands of a colour. It
could not be a picture, because a picture has to come from somewhere -
and a machine with no camera and no image editor has only arithmetic.

So the picture is computed by a program, 'mkwall', written to the disk,
and read back by the compositor at startup. The format is the whole of
the idea:

  "PWAL", the size, 192 colours of three bytes, then one byte per
  pixel - an index into those colours.

Small on purpose: 160 by 120, stretched over the screen. A screen-sized
image is three hundred kilobytes and the kernel's heap is one megabyte;
what is kept here is a fifteenth of that, and every pixel of it covers
a block of the screen. It looks like what it is - a picture, softly.

The colours go in above the desktop's own, so a wallpaper can never
repaint a window - and that is checked here too, along with the thing
that tells a picture from a gradient without knowing what is drawn: a
gradient is the same colour all the way across a row, and a picture is
not.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

WALL_SIZE = 4 + 2 + 2 + 160 * 120 * 3      # "TWL2", size, RGB


def sign_in(q, timeout=60):
    """Signs in on the sign-in screen once it is up. wait_desktop() finds
    that screen by its hint read against the aurora, and here the
    wallpaper is behind it - so it is found by its clock instead: large,
    white, a third of the way down, which no wallpaper here is."""
    end = time.time() + timeout
    while time.time() < end:
        w, h, at = q.screen_raw()
        if w >= 640:
            white = sum(1 for y in range(h // 3 - LOCK_TIME_PX // 2, h // 3, 2)
                        for x in range(w // 2 - 100, w // 2 + 100, 2)
                        if min(at(x, y)) > 250)
            if white > 100:
                q.type_line("thread")
                return True
        time.sleep(0.5)
    return False


def row_colours(at, y, x_from=200, x_to=600, step=8):
    return {at(x, y) for x in range(x_from, x_to, step)}


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")

    try:
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
        try:
            q.wait_serial("thread64>", timeout=60)

            # --- the picture is made by a program in ring 3 ----------
            out = q.command("run mkwall wallpaper.img 1 2", "ended",
                            timeout=90)
            check("the program made a wallpaper", "is ready" in out)
            check("and it ended with no error", "(code 0)" in out)

            fls = q.command("fls", "wallpaper.img", timeout=30)
            size = re.search(r"wallpaper\.img\s+\((\d+) B\)", fls)
            check("the file is on the disk and has the size the format says (%s B)"
                  % (size.group(1) if size else "?"),
                  size is not None and int(size.group(1)) == WALL_SIZE)

            # The blend: the pattern that only exists because the file
            # holds colours rather than a palette of 192. It is the one
            # put on the screen below, so the count further down means
            # something.
            out = q.command("run mkwall wallpaper.img 4 2", "ended",
                            timeout=90)
            check("and the second pattern - the blend - came out too",
                  "is ready" in out and "(code 0)" in out)

            saved = q.sudo("set wallpaper wallpaper.img", "saved")
            check("the setting was saved", "wallpaper = wallpaper.img" in saved)

            # --- and the desktop puts it up -------------------------
            q.type_line("desktop")
            # Started from the console, the desktop picks a mode of its
            # own - measured, not assumed
            # (the desktop is not the aurora now: it is there when its
            # dock is - the first picture, a folder, white a little below
            # its middle)
            def dock_up(at, w, h):
                g = dock_geom(w=w, h=h)
                tx, ty = g["tiles"][0]
                return min(at(tx + g["tile"] // 2,
                              ty + g["tile"] // 2 + 3)) > 200
            check("the sign-in screen came up over the wallpaper",
                  sign_in(q))
            check("the desktop came up", q.wait_desktop(ready=dock_up))
            sw, sh = q.size()
            q.mouse_home()
            # A window over the picture, to see it is still the window
            q.launch_menu("gclock")
            q.mouse_goto(560, 400)
            q.wait_focus(94, 54, timeout=10)
            time.sleep(1)

            log = q.serial()
            check("the kernel read the wallpaper and says which",
                  re.search(r"wallpaper 'wallpaper\.img': 160x120 in colour", log)
                  is not None)

            w, h, at = q.screen_raw()
            # A gradient is one colour all the way across a row; a
            # picture is not. That is the difference, whatever is drawn.
            colours = row_colours(at, 300)
            check("across the screen there is more than one colour (%d)"
                  % len(colours), len(colours) > 3)
            # And it changes down the screen as well - a picture, not a
            # pattern of stripes
            check("and a different set of colours lower down than higher up",
                  row_colours(at, 120) != row_colours(at, 380))

            # And the count: a blend across both axes is one colour per
            # pixel of the file. A wallpaper that carried a palette of
            # 192 could not put more than that on the screen no matter
            # what it was drawn from - so a number well above it says
            # the file really does hold colours.
            blend = set()
            for y in range(0, h - 30, 3):
                for x in range(0, w, 3):
                    blend.add(at(x, y))
            check("the wallpaper has more colours than a palette of 192 could carry "
                  "(%d different)" % len(blend), len(blend) > 400)

            # A window drawn on top of the picture is still the window -
            # its title bar the theme's, along its length - and the dock
            # is still the dock: its first picture (a folder) white in
            # the middle
            title = theme("dark")["title_bg"]
            check("a window on the wallpaper still has its own colour",
                  all(near(at(94 + dx, 54 + 4), title, 4)
                      for dx in range(70, 120, 8)))
            g = dock_geom(n_w=1, w=sw, h=sh)      # (with the clock's tile)
            tx, ty = g["tiles"][0]
            check("and the dock its own",
                  min(at(tx + g["tile"] // 2, ty + g["tile"] // 2 + 3)) > 200)
        finally:
            q.close()

        # --- a wallpaper that is not there ------------------------------
        print("--- a wallpaper that is not there ---")
        slog2 = tempfile.mktemp(suffix=".log")
        q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog2)
        try:
            q2.wait_serial("thread64>", timeout=60)
            q2.sudo("set wallpaper nosuchfile.img", "saved")
            q2.type_line("desktop")
            check("the desktop came up again", q2.wait_desktop(timeout=40))
            q2.mouse_home()
            q2.mouse_goto(560, 400)
            time.sleep(2)

            w, h, at = q2.screen_raw()
            # Back to the aurora, to the last bit
            spots = [(600, 300), (400, 200), (500, 380)]
            check("a missing file does not break the desktop - the aurora is "
                  "back", all(at(x, y) == aurora_px(x, y, w, h)
                              for x, y in spots))
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
