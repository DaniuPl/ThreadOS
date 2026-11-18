#!/usr/bin/env python3
"""ThreadOS smoke test: a program's window on the phone.

The PC's window calls (SYS64_WIN_*) on the ARM64 kernel, where a window
is the whole screen below the status bar (kernelarm64/winmob.c) and the
phone's shell puts it in front (kernelarm64/mobile.c): what Weft's WPE
platform will draw and hear through. The program is libc/tests/wintest.c,
built against musl with the PC's user64/usys64.h. From outside - QEMU's
screen and its tablet:

  the program's window is made, and its first event is its size: the
  screen's logical width and its height less the status bar and the
  home indicator's strip, and the scale it is drawn at - 32/16 at the
  phone's 200%

  what it blits is drawn larger by that much, each pixel a block of the
  panel's - the square's edges fall where twice its own put them - and
  its text (WIN_DRAW) is drawn at the panel's own resolution, read
  back as it was written

  made while the phone is locked, it is shown once the phone is opened,
  under the status bar: what the program drew, colour for colour,
  where it drew it - its top half red, its bottom half blue, the green
  square in the middle, the black one in the corner

  a tap in the square reaches the program as a click, a press and a
  release at that point in the window's own (logical) pixels, and the square
  turns yellow on the screen; a drag reaches it as a press, drags and
  a release along the way

  the swipe home from the bottom edge is the phone's, not the
  program's: home, the window is a tile after the apps, and a tap on
  it brings it back as it was

  a tap in the corner ends the program; its window goes with it, the
  phone goes home, the tile is gone and every page is free again

and the same program from an application package (.tapp,
scripts/mktapp.py) handed over through fw_cfg, as the PC's loader hands
packages over as modules: the kernel takes it - and passes over one
built for the PC - the phone's home has its tile, and a tap on it
starts the package's program, whose window opens; its program runs
under a name only the package has
"""

import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, noarm64, Phone, ARM64_INITRD, _kernel_define

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WINTEST_ELF = os.path.join(os.path.dirname(ARM64_INITRD), "arm64", "libc",
                           "wintest.elf")

K = 2                         # panel pixels to a logical one
MOB = "kernelarm64/mobile.c"
BAR = _kernel_define(MOB, "MOB_BAR_H")
TILE = _kernel_define(MOB, "MOB_TILE")
GRID_X = _kernel_define(MOB, "MOB_GRID_X")
GRID_STEP = _kernel_define(MOB, "MOB_GRID_STEP")
GRID_Y = _kernel_define(MOB, "MOB_GRID_Y")
EDGE = _kernel_define(MOB, "MOB_EDGE")
SAFE = _kernel_define(MOB, "MOB_SAFE_UP")
ROW_STEP = _kernel_define(MOB, "MOB_ROW_STEP")
LW = Phone.LOGICAL[0]


def after_apps():
    """The top left of the first tile after mobile.c's own apps: in
    rows of as many as the width takes (its grid_cols)"""
    src = open(os.path.join(ROOT, MOB)).read()
    body = re.search(r"apps\[\] = \{(.*?)\n\};", src, re.S).group(1)
    k = len(re.findall(r'\{\s*"\w+",\s*APP_\w+', body))
    cols = (LW - GRID_X + GRID_STEP - TILE) // GRID_STEP
    return GRID_X + k % cols * GRID_STEP, GRID_Y + k // cols * ROW_STEP


WIN = "libc/tests/wintest.c"
SQUARE = _kernel_define(WIN, "SQUARE")
CORNER = _kernel_define(WIN, "CORNER")
TEXT_X = _kernel_define(WIN, "TEXT_X")
TEXT_Y = _kernel_define(WIN, "TEXT_Y")
TEXT_PX = _kernel_define(WIN, "TEXT_PX")

RED, BLUE = (0xE0, 0x20, 0x20), (0x20, 0x40, 0xE0)
GREEN, YELLOW, BLACK = (0x20, 0xB0, 0x40), (0xF0, 0xD0, 0x20), (0, 0, 0)
WHITE = (0xFF, 0xFF, 0xFF)
TILE_COLOUR = (0x5E, 0x5C, 0xE6)
PKG_COLOUR = (0xFF, 0x9F, 0x0A)


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


def package(work, name, arch):
    """wintest as an application package's program, under a name only
    the package has"""
    out = os.path.join(work, name + ".tapp")
    subprocess.run([sys.executable, os.path.join(ROOT, "scripts",
                                                 "mktapp.py"), out,
                    "--id", name, "--name", "Pack win", "--version", "3",
                    "--arch", arch, "--exec", "pkgwin",
                    "pkgwin=" + WINTEST_ELF], check=True,
                   capture_output=True)
    return out


def from_package():
    work = tempfile.mkdtemp()
    ours = package(work, "packwin", "aarch64")
    other = package(work, "pcwin", "x86_64")
    q = Phone(cores=2, mib=1024, extra=[
        "-fw_cfg", "name=opt/threados/apps/packwin,file=" + ours,
        "-fw_cfg", "name=opt/threados/apps/pcwin,file=" + other])
    try:
        out = q.wait_serial("[mobile] lock screen", timeout=120)
        check("the kernel takes the package handed over through fw_cfg "
              "(%s)" % next((l for l in out.splitlines()
                             if "Package packwin" in l), "no line"),
              re.search(r"\[ OK \] Package packwin 3: Pack win, \d+ KiB",
                        out) is not None)
        check("and passes over the one built for the PC",
              "a package passed over: its programs are for another "
              "processor" in out)
        q.touch([(180, 560), (180, 420), (180, 300)])
        q.wait_serial("[mobile] home (swipe up)", timeout=20)
        tx, ty = after_apps()
        tile = (tx * K + 8, (ty + TILE // 2) * K)
        check("home has the package's tile after the apps",
              poll_screen(q, lambda at: at(*tile) == PKG_COLOUR))
        q.tap(tx + TILE // 2, ty + TILE // 2)
        out = q.wait_serial('[mobile] window "Win test" (opened)',
                            timeout=60)
        check("a tap on it starts the package's program, and its window "
              "opens",
              re.search(r"\[mobile\] Pack win started from its package "
                        r"\(pid \d+\)", out) is not None and
              re.search(r"\[proc\] pkgwin|\[winmob\] window \d+ for "
                        r"program \d+", out) is not None)
    finally:
        q.close()
        subprocess.run(["rm", "-rf", work])


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
        m = re.search(r"\[wintest\] window (\d+), (\d+)x(\d+) at (\d+)/16, "
                      r"text to (-?\d+), drawn", out)
        ww, wh, k = ((int(m.group(2)), int(m.group(3)), int(m.group(4)))
                     if m else (0, 0, 0))
        check(f"the window is made, and its first event is its size: the "
              f"screen's logical width, its height less the status bar "
              f"and the home indicator's strip ({ww}x{wh}), and its scale "
              f"({k}/16)", (ww, wh, k) == (360, 640 - BAR - SAFE, 16 * K))
        top = BAR * K

        def win(x, y):          # the window's pixels to the panel's
            return x * K, top + y * K

        # Locked: the swipe that opens the phone shows the window
        q.touch([(180, 560), (180, 420), (180, 300)])
        q.wait_serial('[mobile] window "Win test" (opened while locked)',
                      timeout=20)
        mid = (ww // 2, wh // 2)

        # the status bar above it takes the colour of its top row: the
        # black corner and the red, on the whole
        width = ww * K
        bar_colour = tuple(c * (width - CORNER * K) // width for c in RED)

        def drawn(centre):
            return lambda at: (
                at(*win(ww // 2, wh // 4)) == RED and
                at(*win(ww // 2, wh * 3 // 4 + 40)) == BLUE and
                at(*win(*mid)) == centre and
                at(*win(CORNER // 2, CORNER // 2)) == BLACK and
                at(2, 2) == bar_colour)
        check(f"once the phone is opened the window is in front, under the "
              f"status bar: red above, blue below, the green square in the "
              f"middle, the black corner - where the program drew them; the "
              f"status bar the colour of its top row, {bar_colour}",
              poll_screen(q, drawn(GREEN)))
        w, h, at = q.screen_raw()
        sx, sy = win((ww - SQUARE) // 2, (wh - SQUARE) // 2)
        edges = [at(sx - 1, sy + 40), at(sx, sy + 40),
                 at(sx + SQUARE * K - 1, sy + 40), at(sx + SQUARE * K, sy + 40)]
        check(f"drawn twice as large: the square {SQUARE * K} of the "
              f"panel's pixels wide, from {sx} ({edges})",
              edges == [RED, GREEN, GREEN, RED])
        text = q.find_ui(TEXT_X * K, ww * K, top + TEXT_Y * K,
                         px=TEXT_PX * K, face="bold", fg=WHITE, screen=(w, h, at))
        check(f"and its text at the panel's own resolution ({text!r})",
              text == "Hello phone")
        strip = at(ww, (640 - SAFE // 2) * K)
        check(f"under it, in the home indicator's strip, the colour of its "
              f"last row ({strip})", strip == BLUE)

        # A tap in the square, at its middle
        seen = len(q.serial())
        lx, ly = 180, BAR + mid[1]
        q.tap(lx, ly)
        q.wait_serial("[wintest] the square is", timeout=20)
        said = q.serial()[seen:].replace("\r", "")
        px, py = lx, ly - BAR
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
        want = [(x, y - BAR) for x, y in path]
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
        tx, ty = after_apps()
        # (the middle of its left side: clear of the rounded corners and
        # of the letter in the middle)
        tile = (tx * K + 8, (ty + TILE // 2) * K)
        check("the swipe from the bottom edge goes home and is not the "
              "program's", "[wintest]" not in said)
        check("home, the window is a tile after the apps",
              poll_screen(q, lambda at: at(*tile) == TILE_COLOUR))
        q.tap(tx + TILE // 2, ty + TILE // 2)
        q.wait_serial('[mobile] window "Win test" (its tile)', timeout=20)
        check("and a tap on the tile brings it back as it was",
              poll_screen(q, drawn(YELLOW)))

        # The corner ends the program
        seen = len(q.serial())
        q.tap(CORNER // 2, BAR + CORNER // 2)
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
    from_package()
    return summary()


if __name__ == "__main__":
    sys.exit(main())
