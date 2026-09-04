#!/usr/bin/env python3
"""ProtonOS smoke test: a wallpaper behind the desktop.

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

WALL_SIZE = 4 + 2 + 2 + 160 * 120 * 3      # "PWL2", size, RGB


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
            q.wait_for("ProtonOS shell", timeout=40)

            # --- the picture is made by a program in ring 3 ----------
            out = q.command("run mkwall tapeta.img 1 2", "zakonczyl",
                            timeout=90)
            check("program zrobil tapete", "gotowa" in out)
            check("i skonczyl sie bez bledu", "(kod 0)" in out)

            fls = q.command("fls", "tapeta.img", timeout=30)
            size = re.search(r"tapeta\.img\s+\((\d+) B\)", fls)
            check("plik jest na dysku i ma rozmiar z formatu (%s B)"
                  % (size.group(1) if size else "?"),
                  size is not None and int(size.group(1)) == WALL_SIZE)

            # The blend: the pattern that only exists because the file
            # holds colours rather than a palette of 192. It is the one
            # put on the screen below, so the count further down means
            # something.
            out = q.command("run mkwall tapeta.img 4 2", "zakonczyl",
                            timeout=90)
            check("i drugi wzor - przenikanie - tez sie zrobil",
                  "gotowa" in out and "(kod 0)" in out)

            q.type_line("sudo set tapeta tapeta.img")
            time.sleep(1)
            saved = q.command("proton", "zapisane", timeout=30)
            check("ustawienie zapisane", "tapeta = tapeta.img" in saved)

            # --- and the desktop puts it up -------------------------
            q.type_line("desktop")
            time.sleep(6)
            q.mouse_home()
            q.mouse_goto(560, 400)
            time.sleep(2)

            log = q.serial()
            check("jadro wczytalo tapete i mowi, jaka",
                  re.search(r"tapeta 'tapeta\.img': 160x120 w kolorach", log)
                  is not None)

            w, h, at = q.screen_raw()
            # A gradient is one colour all the way across a row; a
            # picture is not. That is the difference, whatever is drawn.
            colours = row_colours(at, 300)
            check("w poprzek ekranu jest wiecej niz jeden kolor (%d)"
                  % len(colours), len(colours) > 3)
            # And it changes down the screen as well - a picture, not a
            # pattern of stripes
            check("i inny zestaw kolorow nizej niz wyzej",
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
            check("tapeta ma wiecej kolorow, niz paleta 192 by udzwignela "
                  "(%d roznych)" % len(blend), len(blend) > 400)

            # The wallpaper's colours live above the desktop's, so a
            # window drawn on top of it is still the window's grey
            check("okno na tapecie ma nadal swoj kolor",
                  at(80, 100) == (227, 227, 227))
            check("a pasek zadan swoj", at(300, h - 6)[0] > 190)
        finally:
            q.close()

        # --- a wallpaper that is not there ------------------------------
        print("--- tapeta, ktorej nie ma ---")
        slog2 = tempfile.mktemp(suffix=".log")
        q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog2)
        try:
            q2.wait_for("ProtonOS shell", timeout=40)
            q2.type_line("sudo set tapeta nieistniejaca.img")
            time.sleep(1)
            q2.command("proton", "zapisane", timeout=30)
            q2.type_line("desktop")
            time.sleep(6)
            q2.mouse_home()
            q2.mouse_goto(560, 400)
            time.sleep(2)

            w, h, at = q2.screen_raw()
            # Back to the gradient: one colour all the way across
            check("brak pliku nie psuje pulpitu - wraca gradient",
                  len(row_colours(at, 300)) == 1)
            check("i tlo dalej ciemnieje ku dolowi",
                  sum(at(600, 40)) > sum(at(600, 400)))
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
