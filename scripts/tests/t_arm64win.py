#!/usr/bin/env python3
"""ThreadOS smoke test: a program's window on the phone.

The PC's window calls (SYS64_WIN_*) on the ARM64 kernel, where a window
is the whole screen below the status bar (kernelarm64/winmob.c) and the
phone's shell puts it in front (kernelarm64/mobile.c): what Weft's WPE
platform will draw and hear through. The program is libc/tests/wintest.c,
built against musl with the PC's user64/usys64.h. From outside - QEMU's
screen and its tablet:

  the program's window is made, and its first event is its size: the
  panel's width and its height less the status bar

  made while the phone is locked, it is shown once the phone is opened,
  under the status bar: what the program drew, colour for colour,
  where it drew it - its top half red, its bottom half blue, the green
  square in the middle, the black one in the corner

  a tap in the square reaches the program as a click, a press and a
  release at that point in the window's own pixels, and the square
  turns yellow on the screen; a drag reaches it as a press, drags and
  a release along the way

  the swipe home from the bottom edge is the phone's, not the
  program's: home, the window is a tile after the apps, and a tap on
  it brings it back as it was

  a tap in the corner ends the program; its window goes with it, the
  phone goes home, the tile is gone and every page is free again
"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, noarm64, Phone, ARM64_INITRD, _kernel_define

K = 2                         # panel pixels to a logical one
MOB = "kernelarm64/mobile.c"
BAR = _kernel_define(MOB, "MOB_BAR_H")
TILE = _kernel_define(MOB, "MOB_TILE")
GRID_X = _kernel_define(MOB, "MOB_GRID_X")
GRID_STEP = _kernel_define(MOB, "MOB_GRID_STEP")
GRID_Y = _kernel_define(MOB, "MOB_GRID_Y")
EDGE = _kernel_define(MOB, "MOB_EDGE")
APPS = 3                      # About, Clock, Draw before the programs
WIN = "libc/tests/wintest.c"
SQUARE = _kernel_define(WIN, "SQUARE")
CORNER = _kernel_define(WIN, "CORNER")

RED, BLUE = (0xE0, 0x20, 0x20), (0x20, 0x40, 0xE0)
GREEN, YELLOW, BLACK = (0x20, 0xB0, 0x40), (0xF0, 0xD0, 0x20), (0, 0, 0)
TILE_COLOUR = (0x5E, 0x5C, 0xE6)


def poll_screen(q, test, timeout=30):
    """Until the screen passes test(at), or the time is up"""
    end = time.time() + timeout
    while time.time() < end:
        try:
            w, h, at = q.screen_raw()
            if test(at):
                return True
        except Exception:
            pass
        time.sleep(0.3)
    return False


def main():
    print("--- a program's window on the phone ---")
    why = noarm64()
    if why:
        print("SKIPPED: " + why)
        return summary()
    if not os.path.exists(ARM64_INITRD):
        print("SKIPPED: no ARM64 initrd (make kernelarm64)")
        return summary()
    q = Phone(cores=2, mib=1024, append="run=wintest")
    try:
        out = q.wait_serial("[wintest] window", timeout=120)
        out = q.wait_serial("drawn", timeout=30)
        m = re.search(r"\[wintest\] window (\d+), (\d+)x(\d+), drawn", out)
        ww, wh = (int(m.group(2)), int(m.group(3))) if m else (0, 0)
        check(f"the window is made, and its first event is its size: the "
              f"panel's width, its height less the status bar ({ww}x{wh})",
              (ww, wh) == (360 * K, (640 - BAR) * K))
        top = BAR * K

        def win(x, y):          # the window's pixels to the panel's
            return x, top + y

        # Locked: the swipe that opens the phone shows the window
        q.touch([(180, 560), (180, 420), (180, 300)])
        q.wait_serial('[mobile] window "Win test" (opened while locked)',
                      timeout=20)
        mid = (ww // 2, wh // 2)

        def drawn(centre):
            return lambda at: (
                at(*win(ww // 2, wh // 4)) == RED and
                at(*win(ww // 2, wh * 3 // 4 + 40)) == BLUE and
                at(*win(*mid)) == centre and
                at(*win(CORNER // 2, CORNER // 2)) == BLACK and
                at(2, 2) == BLACK)
        check("once the phone is opened the window is in front, under the "
              "status bar: red above, blue below, the green square in the "
              "middle, the black corner - where the program drew them",
              poll_screen(q, drawn(GREEN)))

        # A tap in the square, at its middle
        seen = len(q.serial())
        lx, ly = 180, BAR + (mid[1] // K)
        q.tap(lx, ly)
        q.wait_serial("[wintest] the square is", timeout=20)
        said = q.serial()[seen:].replace("\r", "")
        px, py = lx * K, ly * K - top
        events = re.findall(r"^\[wintest\] (click|press|drag|release) "
                            r"(\d+),(\d+)$", said, re.M)
        check(f"a tap reaches the program as a click, a press and a "
              f"release at that point, in the window's pixels ({events})",
              [e[0] for e in events] == ["click", "press", "release"] and
              all((int(x), int(y)) == (px, py) for _, x, y in events))
        check("and the square turns yellow on the screen",
              "[wintest] the square is yellow" in said and
              poll_screen(q, drawn(YELLOW)))

        # A drag: down, along, up - the window's own coordinates, clear of
        # the square and the corner, whose releases mean something
        seen = len(q.serial())
        path = [(250, 150), (290, 190), (330, 230)]
        q.touch(path)
        q.wait_serial("[wintest] release", timeout=20)
        said = q.serial()[seen:].replace("\r", "")
        events = [(k, int(x), int(y)) for k, x, y in re.findall(
            r"^\[wintest\] (click|press|drag|release) (\d+),(\d+)$", said,
            re.M)]
        want = [(x * K, y * K - top) for x, y in path]
        check(f"a drag reaches it as a press, drags and a release along "
              f"the way ({events})",
              events[:2] == [("click",) + want[0], ("press",) + want[0]] and
              ("drag",) + want[-1] in events and
              events[-1] == ("release",) + want[-1] and
              all(e[0] == "drag" for e in events[2:-1]))

        # Home from the bottom edge: the phone's gesture, not the program's
        seen = len(q.serial())
        q.touch([(180, 640 - EDGE // 3), (180, 500), (180, 380)])
        q.wait_serial("[mobile] home (swipe up from the edge)", timeout=20)
        said = q.serial()[seen:]
        tx = GRID_X + APPS * GRID_STEP
        # (the middle of its left side: clear of the rounded corners and
        # of the letter in the middle)
        tile = (tx * K + 8, (GRID_Y + TILE // 2) * K)
        check("the swipe from the bottom edge goes home and is not the "
              "program's", "[wintest]" not in said)
        check("home, the window is a tile after the apps",
              poll_screen(q, lambda at: at(*tile) == TILE_COLOUR))
        q.tap(tx + TILE // 2, GRID_Y + TILE // 2)
        q.wait_serial('[mobile] window "Win test" (its tile)', timeout=20)
        check("and a tap on the tile brings it back as it was",
              poll_screen(q, drawn(YELLOW)))

        # The corner ends the program
        seen = len(q.serial())
        q.tap(CORNER // (2 * K), BAR + CORNER // (2 * K))
        q.wait_serial("[mobile] home (its program ended)", timeout=30)
        out = q.wait_serial("pages free", timeout=30)
        check("a tap in the corner ends the program with 0, and the phone "
              "goes home",
              "[wintest] bye" in out and
              re.search(r"\[proc\] wintest \(pid \d+\) ended with code 0",
                        out) is not None)
        check("its tile is gone with its window",
              poll_screen(q, lambda at: at(*tile) != TILE_COLOUR))
        m = re.search(r"pages free: (\d+) before the programs, (\d+) after",
                      out)
        check(f"and every page it had is free again - its window's too "
              f"({m.group(0) if m else 'nothing said'})",
              m is not None and m.group(1) == m.group(2))
    finally:
        q.close()
    return summary()


if __name__ == "__main__":
    sys.exit(main())
