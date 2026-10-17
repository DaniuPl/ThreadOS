#!/usr/bin/env python3
"""ThreadOS smoke test: a program draws the way the desktop draws.

A program's window used to be a grid of eight-by-eight letters in a
palette of a few dozen colours. Two calls changed that: one draws into
the window with the desktop's own tools - a rounded rectangle, a ring,
a circle, a line, a slope of colour, a translucent fill, TrueType text
- and one hands out the theme's colours (user/udraw.h).

'udtest' puts one of each into a window, white on black, so that how
white a pixel is says how much of it a shape covers, and prints what
the calls answered. This group holds all of it against things that do
not come from the system:

  every shape against its exact area, the pixel cut into 16 x 16 and
  the pieces inside counted - the same yardstick as the kernel's own
  shapes are held to (t_shapes)

  the text read back with FreeType, and its width - what the program
  is told to lay out with - equal to FreeType's advances added up

  the slope running from one colour to the other, the translucent fill
  exactly half way

  a window that is not the program's own refused

  the theme's colours exactly kernel/include/look.h's, and a switch of
  the theme reaching the program while it runs

on both kernels: the call has its own number and its own handler in
long mode.
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
from ptest import in_rrect, in_line, shape_area, check_area

# user/udtest.c: what it draws, where, in its content
TEXT = "ThreadOS 1234"
TEXT_X, TEXT_BASE = 4, 100
SHAPES = [
    # (what, the band of the window it is in: x0, x1, y0, y1, inside)
    ("a rounded rectangle", 0, 48, 0, 36,
     lambda x, y: in_rrect(x, y, 4, 4, 40, 28, 8)),
    ("a ring a pixel thick", 48, 92, 0, 36,
     lambda x, y: in_rrect(x, y, 52, 4, 36, 28, 10) and
     not in_rrect(x, y, 53, 5, 34, 26, 9)),
    ("a circle", 92, 126, 0, 36,
     lambda x, y: in_rrect(x, y, 96, 4, 25, 25, 12.5)),
    ("a line at an odd angle", 126, 198, 0, 36,
     lambda x, y: in_line(x, y, 130.5, 6.5, 190.5, 30.5, 1.5)),
]
GRAD = (4, 40, 60, 24)        # top red, bottom blue
HALF = (70, 40, 30, 24)       # white, 128 of 255 of it, over black

# The theme as udtest prints it, word by word, and where look.h has it
LOOK_KEYS = ["win_bg", "app_field", "app_text", "app_dim", "win_line",
             "app_row", "accent", "accent_text", "app_danger", "app_good"]


def as_int(rgb):
    return (rgb[0] << 16) | (rgb[1] << 8) | rgb[2]


def looks(log):
    """Every set of colours udtest printed, in order"""
    return [[int(v, 16) for v in m.split()]
            for m in re.findall(r"udtest: look((?: [0-9a-f]{6}){11})", log)]


def expected_look(name):
    t = theme(name)
    return [1 if name == "dark" else 0] + [as_int(t[k]) for k in LOOK_KEYS]


def check_window(q, tag, cx, cy):
    w, h, at = q.screen_raw()

    for name, x0, x1, y0, y1, inside in SHAPES:
        ours = [[at(cx + x, cy + y)[0] for x in range(x0, x1)]
                for y in range(y0, y1)]
        ref = shape_area(lambda x, y: inside(x + x0, y + y0), x1 - x0, y1 - y0)
        check_area(tag + name, ours, ref)

    gx, gy, gw, gh = GRAD
    col = [at(cx + gx + gw // 2, cy + gy + j) for j in range(gh)]
    falling = all(a[0] >= b[0] and a[2] <= b[2] for a, b in zip(col, col[1:]))
    check("%sthe slope runs from red at its top to blue at its bottom (%s ... "
          "%s)" % (tag, col[0], col[-1]),
          falling and near(col[0], (255, 0, 0), 12) and
          near(col[-1], (0, 0, 255), 12))

    hx, hy, hw, hh = HALF
    half = set(at(cx + hx + i, cy + hy + j) for j in range(hh)
               for i in range(hw))
    check("%sa fill 128/255 opaque over black is grey half way (%s)"
          % (tag, sorted(half)),
          len(half) == 1 and near(list(half)[0], (128, 128, 128), 1))

    got = q.read_ui(cx + TEXT_X, cy + TEXT_BASE, 190, px=UD_PX,
                    fg=(255, 255, 255))
    check("%sthe text reads back with FreeType ('%s')" % (tag, got),
          got == TEXT)


def check_answers(q, tag, log):
    font = UiFont.shared(UD_PX, "sans")
    adv = sum(font.glyph(ch)[0] for ch in TEXT)
    m = re.search(r"udtest: width (-?\d+) pen (-?\d+) foreign (-?\d+)", log)
    check("%sthe program said what the calls answered" % tag, m is not None)
    if not m:
        return
    width, pen, foreign = (int(v) for v in m.groups())
    check("%sthe width it is told is FreeType's advances added up (%d, "
          "FreeType %d)" % (tag, width, (adv + 63) >> 6),
          width == (adv + 63) >> 6)
    check("%sand the pen ends where the text does (%d)" % (tag, pen),
          pen == ((TEXT_X << 6) + adv + 63) >> 6)
    check("%sa window that is not its own is refused (%d)" % (tag, foreign),
          foreign == -1)


def main():
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = Qemu(KERNEL, INITRD, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()
            q.type_line("run udtest")
            time.sleep(4)
            q.mouse_goto(600, 300)
            time.sleep(1.5)

            # The window opens in slot 2 of the cascade
            cx, cy = 94 + 1, 54 + CONTENT_DY
            check_window(q, "", cx, cy)
            log = q.serial()
            check_answers(q, "", log)

            seen = looks(log)
            check("the theme's colours are look.h's dark ones (%s)"
                  % (["%06x" % v for v in seen[0]] if seen else "-"),
                  bool(seen) and seen[0] == expected_look("dark"))

            # --- a switch of the theme reaches the program -------------
            q.click_desktop()             # the keys back to the shell
            time.sleep(1)
            q.type_line("sudo set theme light")
            time.sleep(1)
            q.type_line("thread")
            got = []
            for _ in range(20):
                time.sleep(0.5)
                got = looks(q.serial())
                if len(got) >= 2:
                    break
            check("the program learns of the light theme while it runs (%s)"
                  % (["%06x" % v for v in got[-1]] if got else "-"),
                  len(got) >= 2 and got[-1] == expected_look("light"))
        finally:
            q.close()
    finally:
        if os.path.exists(slog):
            os.unlink(slog)

    print("--- the same calls on the 64-bit kernel ---")
    reason = no64()
    if reason:
        print("SKIPPED (64): " + reason)
        return summary()
    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    try:
        check("(64) the disc with the 64-bit kernel builds",
              make_iso64(d, iso, graphics=True))
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=120)
            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=40)
            q.wait_desktop(40)
            q.mouse_home()
            q.type_line("run udtest")
            time.sleep(4)
            q.mouse_goto(700, 400)
            time.sleep(1.5)
            cx, cy = 94 + 1, 54 + CONTENT_DY
            check_window(q, "(64) ", cx, cy)
            log = q.serial()
            check_answers(q, "(64) ", log)
            seen = looks(log)
            check("(64) the theme's colours are look.h's dark ones",
                  bool(seen) and seen[0] == expected_look("dark"))
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
