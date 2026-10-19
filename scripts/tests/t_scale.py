#!/usr/bin/env python3
"""ThreadOS smoke test: the panel's own resolution, and a scale drawn
sharp.

A desktop laid out in pixels was right while a screen was 640x480. A
laptop's panel is 1920x1080 on fifteen inches, and there two things
have to be true at once: the desktop has to use the panel's own mode -
anything else the panel scales, and a scaled picture is a soft one -
and it has to come out large enough to read. The second used to be
pixel doubling: everything drawn small and every pixel put down as a
square of four. Now the compositor draws AT the panel's size - every
edge worked out in the panel's pixels, every letter rendered from its
outline one and a half or two times larger - and only the layout is in
the smaller, logical pixels.

What is asserted, against things that do not come from the system:

  with the screen saying what it is (EDID, as QEMU's VGA does), the
  desktop starts in that mode - the screen QEMU shows is that size,
  and the aurora covers all of it, painted at that size
  the 'resolution' setting wins over what the screen says - changed
  while the desktop runs, kept for the next start, and 'native' gives
  the screen's own mode back

  with nothing said (as on a board whose card this system has no
  driver for), a panel of 1080 rows gets one and a half: the desktop
  is laid out at 1280x720 and drawn at 1920x1080
  and it is drawn, not blown up: a window's title, and the number in
  the calculator's display, are FreeType's letters at one and a half
  times their size, pixel for pixel - which no enlargement of the
  small letters could be
  the pointer still lands where the layout says: clicked keys of the
  calculator add up, typed ones too

  at two, the counter-check that it is not doubling: in a window's
  title, the panel's pixels are not equal in squares of four

  and changed while it runs, from Appearance: the desktop takes up one
  and a half within a second, a program that was open is drawn again
  at the new size - its title and its numbers FreeType's at one and a
  half - and still answers its keys; 'Auto' puts it back

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
from ptest import _kernel_define as define

DARK = theme("dark")
C = "user/gcalc.c"


def boot(disk, slog, edid, settings):
    """A machine in text mode, the settings made, then the desktop"""
    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog, edid=edid)
    q.wait_for("ThreadOS shell", timeout=40)
    for key, value in settings:
        q.type_line("sudo set %s %s" % (key, value))
        time.sleep(1)
        q.command("thread", "saved", timeout=30)
    q.type_line("desktop")
    time.sleep(9)
    q.mouse_home()
    q.mouse_goto(300, 200)
    time.sleep(1)
    return q


def mode(q):
    """What the machine says it draws at: logical size, scale, panel"""
    said = q.command("screen", "frames", timeout=10)
    m = re.search(r"mode:\s+(\d+)x(\d+), [^,]+, scale (\d+)%, panel (\d+)x(\d+)",
                  said)
    return tuple(int(v) for v in m.groups()) if m else None


def aurora_covers(at, w, h):
    """Where nothing stands - down the right-hand side, between the bar
    and the dock - the panel is the aurora painted at the panel's size"""
    spots = [(x, y) for x in range(w * 3 // 4, w, 23)
             for y in range(h // 8, h * 3 // 4, 23)]
    same = sum(1 for x, y in spots if at(x, y) == aurora_px(x, y, w, h))
    return same, len(spots)


def P(v, k):
    return v * k // 16


def ppx(px, k):
    return (px * k + 8) // 16


def native():
    """The screen says 1920x1080; then the setting says otherwise"""
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = boot(disk, slog, (1920, 1080), [])
        try:
            m = mode(q)
            check("the screen's own mode: the panel is 1920x1080 (%s)" % (m,),
                  m is not None and m[3:] == (1920, 1080))
            # QEMU's EDID says a hundred dots an inch, which is about
            # what the desktop was laid out for: no scale
            check("and at its density no scale is needed (%s)" % (m,),
                  m is not None and m[:3] == (1920, 1080, 100))
            w, h, at = q.screen_raw()
            check("QEMU shows a screen of that size (%dx%d)" % (w, h),
                  (w, h) == (1920, 1080))
            same, n = aurora_covers(at, w, h)
            check("and the aurora is painted at that size (%d/%d samples)"
                  % (same, n), same == n)

            # The setting, while the desktop runs: the mode changes
            # under it, and everything is painted for the new size
            q.type_line("sudo set resolution 1280x960")
            time.sleep(1)
            q.command("thread", "saved", timeout=30)
            time.sleep(3)
            m = mode(q)
            w, h, at = q.screen_raw()
            check("'resolution 1280x960' changes the mode while the desktop "
                  "runs (%s, %dx%d)" % (m, w, h),
                  m is not None and m[3:] == (1280, 960) and
                  (w, h) == (1280, 960))
            same, n = aurora_covers(at, w, h)
            check("and the aurora is painted again for it (%d/%d samples)"
                  % (same, n), same == n)
        finally:
            q.close()

        q = boot(disk, slog, (1920, 1080), [])
        try:
            m = mode(q)
            w, h, at = q.screen_raw()
            check("kept: the next start is in that mode too, whatever the "
                  "screen says (%s, %dx%d)" % (m, w, h),
                  m is not None and m[3:] == (1280, 960) and
                  (w, h) == (1280, 960))
            q.type_line("sudo set resolution native")
            time.sleep(1)
            q.command("thread", "saved", timeout=30)
            time.sleep(3)
            m = mode(q)
            w, h, at = q.screen_raw()
            check("and 'native' gives the screen its own mode back (%s, "
                  "%dx%d)" % (m, w, h),
                  m is not None and m[3:] == (1920, 1080) and
                  (w, h) == (1920, 1080))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)


def calc_frame(lw, lh):
    cw, ch = define(C, "W0"), define(C, "H0")
    x, y = cascade(2, cw + 2, ch + TITLE_H + 2, w=lw, h=lh)
    return x, y, cw, ch


def one_and_a_half():
    """Nothing said about the screen; 1080 rows decide the scale"""
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = boot(disk, slog, None, [("resolution", "1920x1080")])
        try:
            m = mode(q)
            check("1080 rows and nothing said: one and a half, the desktop "
                  "1280x720 (%s)" % (m,),
                  m is not None and m == (1280, 720, 150, 1920, 1080))
            if not m:
                return
            k = 24
            lw, lh = 1280, 720
            w, h, at = q.screen_raw()
            same, n = aurora_covers(at, w, h)
            check("the aurora painted at the panel's size, not stretched "
                  "(%d/%d samples)" % (same, n), same == n)

            q.type_line("run gcalc")
            time.sleep(4)
            q.mouse_goto(900, 600)
            time.sleep(1)
            x, y, cw, ch = calc_frame(lw, lh)
            scr = q.screen_raw()
            got = q.find_ui(P(x + LIGHT_X + 2 * LIGHT_STEP + LIGHT_D + 6, k),
                            P(x + cw, k),
                            P(title_baseline(y), k), px=ppx(TITLE_PX, k),
                            face="bold", fg=DARK["title_text"], screen=scr)
            check("the title is FreeType's bold at one and a half times its "
                  "size, read pixel for pixel ('%s')" % got,
                  got == "Calculator")
            cx, cy = x + 1, y + CONTENT_DY

            def display():
                return q.find_ui(P(cx + 20, k), P(cx + cw - 10, k),
                                 P(cy + define(C, "DISP_BASE"), k),
                                 px=ppx(define(C, "DISP_PX"), k),
                                 fg=DARK["app_text"])
            got = display()
            check("and so is what the program draws: its display ('%s')"
                  % got, got == "0")

            # The pointer and the keys go by the layout
            def key(row, col):
                kx = define(C, "KEY_X0") + col * (define(C, "KEY_W") +
                                                  define(C, "KEY_GAP"))
                ky = define(C, "KEY_Y0") + row * (define(C, "KEY_H") +
                                                  define(C, "KEY_GAP"))
                q.click(cx + kx + define(C, "KEY_W") // 2,
                        cy + ky + define(C, "KEY_H") // 2)
                time.sleep(0.3)
            for rc in ((1, 0), (3, 3), (1, 1), (4, 3)):   # 7 + 8 =
                key(*rc)
            q.mouse_goto(900, 600)
            time.sleep(1)
            got = display()
            check("clicks land where the layout says: 7 + 8 = '%s'" % got,
                  got == "15")
            for ch_ in "c12*3\n":
                q.sendkey(Qemu.KEYNAMES.get(ch_, ch_))
                time.sleep(0.12)
            time.sleep(1)
            got = display()
            check("and keys reach it: 12 * 3 = '%s'" % got, got == "36")
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)


def twice():
    """Asked for two: the layout of 640x480 on a 1280x960 panel"""
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = boot(disk, slog, (1280, 960), [("scale", "200")])
        try:
            m = mode(q)
            check("asked for two: the desktop 640x480 on 1280x960 (%s)"
                  % (m,), m is not None and m == (640, 480, 200, 1280, 960))
            if not m:
                return
            k = 32
            q.launch_dock(4)
            time.sleep(3)
            w, h, at = q.screen_raw()
            # The first application window's title bar, letters and lights
            got = q.find_ui(P(94 + LIGHT_X + 2 * LIGHT_STEP + LIGHT_D + 6, k),
                            P(94 + TERM_W + 2, k),
                            P(title_baseline(54), k), px=ppx(TITLE_PX, k),
                            face="bold", screen=(w, h, at))
            pairs = same = 0
            for yy in range(P(54, k), P(54 + TITLE_H, k), 2):
                for xx in range(P(94, k), P(94 + TERM_W + 2, k), 2):
                    pairs += 1
                    a = at(xx, yy)
                    if a == at(xx + 1, yy) == at(xx, yy + 1) == \
                       at(xx + 1, yy + 1):
                        same += 1
            check("not doubled: in a title bar the panel's pixels are not "
                  "squares of four (%d/%d are)" % (same, pairs),
                  pairs > 100 and same < pairs * 9 // 10)
            check("and its title reads at twice the size ('%s')" % got,
                  got.startswith("Term"))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)


A = "user/gappear.c"


def live():
    """At 100% on a 1920x1080 screen; then 150% picked in Appearance"""
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = boot(disk, slog, (1920, 1080), [])
        try:
            q.type_line("run gcalc")
            time.sleep(4)
            q.click(1900, 300)                   # the keys back to the shell
            time.sleep(0.5)
            q.type_line("run gappear")
            time.sleep(4)
            q.click(1900, 300)
            time.sleep(0.5)
            check("before: 100%% on the 1920x1080 screen (%s)" % (mode(q),),
                  mode(q) == (1920, 1080, 100, 1920, 1080))
            aw, ah = define(A, "W0"), define(A, "H0")
            ax, ay = cascade(3, aw + 2, ah + TITLE_H + 2, w=1920, h=1080)
            pad, sc_w = define(A, "PAD"), define(A, "SC_W")
            q.click(ax + 1 + pad + 3 * (sc_w + 8) + sc_w // 2,
                    ay + CONTENT_DY + define(A, "SC_Y") + define(A, "SC_H") // 2)
            time.sleep(1)
            for ch_ in "thread\n":
                q.sendkey(Qemu.KEYNAMES.get(ch_, ch_))
                time.sleep(0.12)
            time.sleep(3)
            q.sendkey("alt-f4")                  # Appearance, off the calculator
            time.sleep(2)
            q.click(1200, 300)                   # the shell, to ask it
            time.sleep(0.5)
            m = mode(q)
            check("'150%%' in Appearance: the desktop is 1280x720 at once (%s)"
                  % (m,), m == (1280, 720, 150, 1920, 1080))
            if m != (1280, 720, 150, 1920, 1080):
                return
            k = 24
            x, y, cw, ch = calc_frame(1280, 720)
            q.mouse_goto(900, 600)
            time.sleep(1)
            scr = q.screen_raw()
            got = q.find_ui(P(x + LIGHT_X + 2 * LIGHT_STEP + LIGHT_D + 6, k),
                            P(x + cw, k), P(title_baseline(y), k),
                            px=ppx(TITLE_PX, k), face="bold", screen=scr)
            check("the calculator that was open is there, its title at one "
                  "and a half ('%s')" % got, got == "Calculator")
            cx, cy = x + 1, y + CONTENT_DY

            def display():
                return q.find_ui(P(cx + 20, k), P(cx + cw - 10, k),
                                 P(cy + define(C, "DISP_BASE"), k),
                                 px=ppx(define(C, "DISP_PX"), k),
                                 fg=DARK["app_text"])
            got = display()
            check("and the program drew itself again at the new size ('%s')"
                  % got, got == "0")
            q.click(cx + cw // 2, cy + 40)       # its display: the focus
            time.sleep(0.5)
            for ch_ in "7*6\n":
                q.sendkey(Qemu.KEYNAMES.get(ch_, ch_))
                time.sleep(0.12)
            q.mouse_goto(900, 600)               # the pointer off the display
            time.sleep(1)
            got = display()
            check("it still counts: 7 * 6 = '%s'" % got, got == "42")

            q.click(1200, 300)
            time.sleep(0.5)
            q.type_line("sudo set scale 0")
            time.sleep(1)
            q.command("thread", "saved", timeout=30)
            time.sleep(3)
            m = mode(q)
            check("'Auto' again: the screen's own density, 100%% (%s)" % (m,),
                  m == (1920, 1080, 100, 1920, 1080))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)


def main():
    native()
    one_and_a_half()
    twice()
    live()
    return summary()


sys.exit(main())
