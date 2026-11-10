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

The wallpaper is made and set at the text shell, and the shell's
'desktop' command then starts the desktop at its sign-in; the picture
is read off the signed-in desktop.

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


def row_colours(at, y, x_from=200, x_to=600, step=8):
    return {at(x, y) for x in range(x_from, x_to, step)}


def hue_rgb(hue, v):
    """user/mkwall.c's hue_rgb()"""
    return [(0, v * 3 // 4, v), (0, 0, v), (v * 3 // 5, 0, v),
            (0, v, 0), (v, v * 2 // 5, 0), (v, v, v)][hue % 6]


def blend_px(x, y, w, h, hue=2):
    """The screen pixel (x, y) of 'mkwall <file> 4 <hue>', the blend, by
    user/mkwall.c's arithmetic - every number in it is positive, so
    Python's division is C's - stretched the way paint_background() in
    kernel/desktop.c does it: each screen pixel takes the picture's
    pixel it falls in."""
    pw, ph = 160, 120
    sx, sy = x * pw // w, y * ph // h
    v = 40 + (ph - 1 - sy) * 215 // (ph - 1)
    mix = sx * 255 // (pw - 1)
    a, b = hue_rgb(hue, v), hue_rgb(hue + 3, v)
    return tuple((a[i] * (255 - mix) + b[i] * mix) // 255 for i in range(3))


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")

    try:
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
        try:
            q.wait_for("thread64>", timeout=40)

            # --- the picture is made by a program in ring 3 ----------
            # The shell waits for it and then says how it ended
            out = q.command("run mkwall wallpaper.img 1 2", "[mkwall] done",
                            timeout=90)
            check("the program made a wallpaper", "is ready" in out)
            check("and it ended with no error",
                  "[mkwall] done, exit code 0 " in out)

            fls = q.command("fls", "wallpaper.img", timeout=30)
            size = re.search(r"wallpaper\.img\s+\((\d+) B\)", fls)
            check("the file is on the disk and has the size the format says (%s B)"
                  % (size.group(1) if size else "?"),
                  size is not None and int(size.group(1)) == WALL_SIZE)

            # The blend: the pattern that only exists because the file
            # holds colours rather than a palette of 192. It is the one
            # put on the screen below, so the count further down means
            # something.
            out = q.command("run mkwall wallpaper.img 4 2", "[mkwall] done",
                            timeout=90)
            check("and the second pattern - the blend - came out too",
                  "is ready" in out and "[mkwall] done, exit code 0 " in out)

            # The first 'sudo' on this fresh disk: it is what saves the
            # admin account the desktop's sign-in below asks for
            q.command("sudo set wallpaper wallpaper.img", "password:",
                      timeout=30)
            saved = q.command("thread", "saved", timeout=30)
            check("the setting was saved", "wallpaper = wallpaper.img" in saved)

            # --- and the desktop puts it up -------------------------
            q.type_line("desktop")
            said = re.compile(r"wallpaper 'wallpaper\.img': 160x120 in colour")
            deadline = time.time() + 60
            while time.time() < deadline and not said.search(q.serial()):
                time.sleep(0.3)
            check("the kernel read the wallpaper and says which",
                  said.search(q.serial()) is not None)

            # The desktop starts at its sign-in, over the wallpaper, with
            # the disk's account named and its password field waiting.
            # Its words cannot be read off a wallpaper the test does not
            # know pixel by pixel, but its clock can be seen: large white
            # figures in the upper middle of the screen. Keys sent before
            # it is up do not all reach it, so the password goes in once
            # it is there. Signed in, the dock comes up - its first tile
            # with the white of its picture in the middle.
            def white_figures():
                w, h, at = q.screen_raw()
                if w < 640:
                    return 0
                return sum(1 for y in range(100, 170, 2)
                           for x in range(200, 440, 2)
                           if at(x, y) == (255, 255, 255))
            deadline = time.time() + 60
            while time.time() < deadline and white_figures() < 300:
                time.sleep(0.5)
            q.mouse_home()
            q.mouse_goto(560, 400)
            q.type_line("thread")
            signed_in = False
            deadline = time.time() + 30
            while time.time() < deadline and not signed_in:
                w, h, at = q.screen_raw()
                signed_in = min(at(*dock_tile(0))) > 200
                time.sleep(0.5)
            check("signed in, the desktop comes up over the wallpaper",
                  signed_in)

            # The bare desktop right of the three shortcuts, between the
            # bar and the dock, and away from the pointer. Read until it
            # shows the file's picture: the frame after the sign-in may
            # still be on its way.
            spots = [(x, y) for y in range(150, work_bottom() - 10, 16)
                     for x in range(300, 630, 16)
                     if not (548 <= x < 592 and 388 <= y < 432)]
            deadline = time.time() + 10
            while True:
                w, h, at = q.screen_raw()
                off = [(x, y) for x, y in spots
                       if at(x, y) != blend_px(x, y, w, h)]
                if not off or time.time() > deadline:
                    break
                time.sleep(0.5)
            # The aurora, or the sign-in over it, has many colours too:
            # the checks below would pass on either. Behind a signed-in
            # desktop, pixel for pixel the picture mkwall made, is what
            # says it is the wallpaper they are counted on.
            check("behind the desktop is the file's picture, pixel for pixel "
                  "(%d of %d differ%s)"
                  % (len(off), len(spots),
                     ", the first at %s: %s, not %s" % (
                         off[0], at(*off[0]), blend_px(*off[0], w, h))
                     if off else ""),
                  signed_in and not off)

            # A gradient is one colour all the way across a row; a
            # picture is not. That is the difference, whatever is drawn.
            colours = row_colours(at, 300)
            check("across the screen there is more than one colour (%d)"
                  % len(colours), signed_in and len(colours) > 3)
            # And it changes down the screen as well - a picture, not a
            # pattern of stripes
            check("and a different set of colours lower down than higher up",
                  signed_in and row_colours(at, 120) != row_colours(at, 380))

            # And the count: a blend across both axes is one colour per
            # pixel of the file. A wallpaper that carried a palette of
            # 192 could not put more than that on the screen no matter
            # what it was drawn from - so a number well above it says
            # the file really does hold colours. Only the file's own
            # pixels are counted: the aurora has thousands of colours
            # of its own, and so do the bar's glass and the dock.
            blend = set()
            for y in range(0, h - 30, 3):
                for x in range(0, w, 3):
                    px = at(x, y)
                    if px == blend_px(x, y, w, h):
                        blend.add(px)
            check("the wallpaper has more colours than a palette of 192 could carry "
                  "(%d different)" % len(blend),
                  signed_in and len(blend) > 400)

            # A window drawn on top of the picture is still the window,
            # and the dock is still the dock. The settings open in the
            # first place of the cascade, (94,54), their background
            # bare at the right of their pane.
            q.launch_dock("gsettings")
            q.mouse_goto(560, 400)
            body = (94 + 1 + GSET_W - 3, 54 + 150)
            deadline = time.time() + 15
            while time.time() < deadline:
                w, h, at = q.screen_raw()
                if near(at(*body), theme("dark")["win_bg"], 3):
                    break
                time.sleep(0.5)
            check("a window on the wallpaper still has its own colour (%s)"
                  % (at(*body),), near(at(*body), theme("dark")["win_bg"], 3))
            check("and the dock its own",
                  min(at(*dock_tile(0, n_w=1))) > 200)
        finally:
            q.close()

        # --- a wallpaper that is not there ------------------------------
        print("--- a wallpaper that is not there ---")
        slog2 = tempfile.mktemp(suffix=".log")
        q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog2)
        try:
            q2.wait_for("thread64>", timeout=40)
            q2.command("sudo set wallpaper nosuchfile.img", "password:",
                       timeout=30)
            q2.command("thread", "saved", timeout=30)
            q2.type_line("desktop")
            # Past the sign-in, to the bare desktop - which is only seen
            # as such when the aurora is back
            try:
                up = q2.wait_desktop(timeout=60)
            except RuntimeError:
                up = False
            q2.mouse_home()
            q2.mouse_goto(560, 400)

            # Back to the aurora, to the last bit. The pointer has just
            # moved, so the frame that shows it where it is now may
            # still be on its way.
            spots = [(600, 300), (400, 200), (500, 380)]
            deadline = time.time() + 15
            while True:
                w, h, at = q2.screen_raw()
                off = [(x, y) for x, y in spots
                       if at(x, y) != aurora_px(x, y, w, h)]
                if not off or time.time() > deadline:
                    break
                time.sleep(0.5)
            check("a missing file does not break the desktop - the aurora is "
                  "back%s" % (" (%s: %s, not %s)" % (
                      off[0], at(*off[0]), aurora_px(*off[0], w, h))
                      if off else ""),
                  up and not off)
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
