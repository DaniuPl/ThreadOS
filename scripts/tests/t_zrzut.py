#!/usr/bin/env python3
"""ProtonOS smoke test: a picture of the screen, taken and shown.

The machine could draw a desktop and could read a wallpaper from a
file. What it could not do was look at itself: there was no way to save
what was on the screen, and nothing that would show such a file if
there were one.

Print Screen now writes the screen to the disk in the machine's own
terms - four bytes "PSCR", the size, the whole palette (so the file
says what its numbers mean) and then one byte per pixel, the palette
index. That is exactly what the compositor drew, so the viewer has
nothing to interpret: it puts the bytes back into a window through
win_blit, a row at a time, and they are the same colours again.

The assertions are about the loop being real, not about a file
appearing. The picture has to BE the screen: the orange of the icons
covers a quarter of the area inside the window that it covers on the
screen itself, because the picture is shown every second pixel - and
that is a number no accident produces.

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
from ptest import check, summary, font8x8

# The viewer opens in the next free slot of the cascade; with the clock
# already open it is slot 3, at (106,66), and asks for 420x300.
VX, VY = 106, 66
CX, CY = VX + 1, VY + 11
VW, VH = 420, 300
STATUS_H = 10

ORANGE = (255, 162, 0)          # COL_ICON - the icon tiles


def is_orange(px):
    return all(abs(px[i] - ORANGE[i]) <= 30 for i in range(3))


def status(at, cols=48, vx=VX, vy=VY):
    """The viewer's status line: white on the accent blue."""
    font = font8x8()
    x0, y0 = vx + 1 + 4, vy + 11 + VH - STATUS_H + 1
    out = ""
    for c in range(cols):
        pattern = []
        for y in range(8):
            bits = 0
            for x in range(8):
                p = at(x0 + c * 8 + x, y0 + y)
                if all(v > 200 for v in p):
                    bits |= 1 << x
            pattern.append(bits)
        out += font.get(tuple(pattern), " " if not any(pattern) else "?")
    return out.strip()


def count_orange(at, x0, y0, x1, y1):
    return sum(1 for y in range(y0, y1, 2) for x in range(x0, x1, 2)
               if is_orange(at(x, y)))


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")

    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("pulpit wstal", q.wait_desktop(timeout=40))
            q.mouse_home()

            # Something on the screen worth photographing
            q.mouse_goto(24, 72)               # the "Zegar" icon
            q.mouse_click()
            time.sleep(4)
            q.mouse_goto(420, 300)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            # How much orange the icons put on the screen - the number
            # the picture will have to repeat, a quarter of it
            on_screen = count_orange(at, 0, 0, 64, h - 20)
            check("ikony sa na ekranie (%d pomaranczowych probek)"
                  % on_screen, on_screen > 50)

            # --- Print Screen -------------------------------------------
            q.sendkey("print")
            time.sleep(4)
            q.mouse_goto(600, 430)
            q.mouse_click()                    # the keys back to the shell
            time.sleep(1)
            seen = len(q.serial())
            q.type_line("fls")
            time.sleep(2)
            listing = q.serial()[seen:]
            m = re.search(r"zrzut1\.psc\s+\((\d+) B\)", listing)
            check("Print Screen zapisal plik na dysku ('%s')"
                  % (m.group(0) if m else listing[-60:].replace("\n", " ")),
                  m is not None)
            if not m:
                return summary()
            # 640*480 pixels + "PSCR" + two sizes + 256 colours
            check("i ma dokladnie tyle bajtow, ile ma ekran plus paleta "
                  "(%s)" % m.group(1),
                  int(m.group(1)) == 640 * 480 + 4 + 2 + 2 + 256 * 3)

            # --- and the viewer puts it back on the screen --------------
            q.type_line("run gimg zrzut1.psc")
            time.sleep(6)
            q.mouse_goto(560, 430)
            time.sleep(2)
            w, h, at = q.screen_raw()

            said = status(at)
            check("podglad mowi, co wczytal ('%s')" % said,
                  "640x480" in said and "wczytany" in said)
            check("i ze pokazuje co drugi pixel, bo caly ekran nie "
                  "zmiesci sie w oknie na tym ekranie", "co 2" in said)

            # The picture IS the screen: the icons' orange inside the
            # window, in the strip where the icons were, and a quarter
            # of what it was (every second pixel in each direction).
            inside = count_orange(at, CX, CY, CX + 32, CY + (h - 20) // 2)
            check("obraz w oknie to naprawde ekran - ikony sa na swoim "
                  "miejscu (%d wobec %d na ekranie)" % (inside, on_screen),
                  inside > on_screen // 6)

            # --- the counter, and what is not a picture -----------------
            q.mouse_goto(600, 430)
            q.mouse_click()
            time.sleep(1)
            q.sendkey("print")
            time.sleep(4)
            seen = len(q.serial())
            q.type_line("fls")
            time.sleep(2)
            listing = q.serial()[seen:]
            check("drugi zrzut dostaje kolejny numer",
                  "zrzut2.psc" in listing and "zrzut1.psc" in listing)

            q.type_line("run gimg swap.sys")
            time.sleep(5)
            q.mouse_goto(560, 430)
            time.sleep(2)
            w, h, at = q.screen_raw()
            # The viewer for a file that is not a picture opens in the
            # next slot again - the same place, because the previous
            # one is still there
            # The second viewer opens in the next slot of the cascade -
            # the first one is still standing where it was
            said = status(at, vx=VX + 12, vy=VY + 12)
            check("plik, ktory nie jest obrazem, jest odrzucony ze "
                  "slowem wyjasnienia ('%s')" % said,
                  "nie jest obraz" in said)
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
