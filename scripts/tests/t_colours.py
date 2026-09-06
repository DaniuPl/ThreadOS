#!/usr/bin/env python3
"""ProtonOS smoke test: colours - as many as the screen really has.

This machine drew in palette indices from its first pixel. That made
sense while the screen WAS a palette - mode 13h, two hundred and fifty
six entries and a DAC that took six bits a channel. The framebuffer a
loader hands over today holds colours, thirty-two bits of them, and the
palette had become a ceiling the hardware did not ask for.

So there are two kinds of window now. One holds indices, as every
program here always did, and none of them had to change. The other
holds colours - a program asks for it once, and then a photograph is a
photograph rather than a photograph seen through thirty-six crayons.

What is asserted here is the difference, in pixels on the screen:

  the mode really is a colour mode, and the machine says so
  a gradient from outside comes out with far more distinct colours than
  a palette of two hundred and fifty six shared with a desktop could
  ever have given it - the number is counted, not eyeballed
  and it rises where it should rise, so "more colours" did not come at
  the price of the right ones
  everything the machine draws for itself is still a few dozen colours,
  because none of those programs had to change
  the compositor writes to the card only what changed: an idle desktop
  is redrawn thirty times a second and sends nothing at all

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
VW, VH = 420, 300
STATUS_H = 10


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


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")

    http_port, stop_http = start_http_server()
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()

            # --- the machine says what kind of screen it has ------------
            q.mouse_goto(600, 430)
            q.mouse_click()
            time.sleep(1)
            seen = len(q.serial())
            q.type_line("screen")
            time.sleep(2)
            said = q.serial()[seen:]
            check("the screen says the mode is in colour ('%s')"
                  % said.replace("\n", " ")[:70],
                  "true colour" in said and "640x480" in said)

            # --- nothing moves, nothing goes to the card ----------------
            # The compositor redraws the whole frame thirty times a
            # second whether anything moved or not. What must NOT happen
            # thirty times a second is a megabyte crossing the bus.
            time.sleep(3)
            seen = len(q.serial())
            q.type_line("screen")
            time.sleep(2)
            said = q.serial()[seen:]
            m = re.search(r"last frame:      (\d+) rows", said)
            check("the compositor writes to the card only what changed "
                  "('%s')" % (m.group(0) if m else said[-60:]),
                  m is not None and int(m.group(1)) < 40)

            # --- a picture in real colours ------------------------------
            q.mouse_goto(24, 72)               # the "Clock" icon, for the slot
            q.mouse_click()
            time.sleep(4)

            # Everything this machine draws for ITSELF - the background,
            # the bars, the icons, a window that never asked for
            # anything else - still lives in the palette. Counted over
            # the whole screen, before any picture is on it, so there is
            # no geometry to get wrong: a few dozen colours, not
            # thousands.
            w, h, at = q.screen_raw()
            own = set()
            for y in range(0, h, 4):
                for x in range(0, w, 4):
                    own.add(at(x, y))
            check("a desktop drawn in indices has a handful of them (%d different "
                  "on the whole screen)" % len(own), 1 < len(own) < 100)

            q.mouse_goto(600, 430)             # the keys back to the shell
            q.mouse_click()
            time.sleep(1)
            seen = len(q.serial())
            q.type_line("run httpget 10.0.2.2 %d /gradient.jpg foto.jpg"
                        % http_port)
            time.sleep(6)
            got = q.serial()[seen:]
            check("the machine fetches a picture off the network",
                  "HTTP 200" in got and "has ended (code 0)" in got)

            q.type_line("run gimg foto.jpg")
            time.sleep(7)
            q.mouse_goto(600, 200)
            time.sleep(2)
            w, h, at = q.screen_raw()
            said = status(at)
            check("the viewer loaded the picture ('%s')" % said,
                  "64x48" in said and "loaded" in said)

            # How many DIFFERENT colours the picture put on the screen.
            # The whole point of the number: three thousand pixels of
            # gradient hold about three thousand colours, and a screen
            # speaking in indices cannot show more than the palette
            # has - which is two hundred and fifty six, most of them
            # already spent on the desktop. Anything above five hundred
            # is a thing the old path could not do at all.
            cx, cy = VX + 1, VY + 11
            seen_cols = set()
            for y in range(0, 48):
                for x in range(0, 64):
                    seen_cols.add(at(cx + x, cy + y))
            check("the picture has more shades than the palette ever had "
                  "(%d different)" % len(seen_cols), len(seen_cols) > 500)

            # More colours is worth nothing if they are the wrong ones.
            # The picture rises in red from left to right and in green
            # from top to bottom, so the pixels have to do the same -
            # read at eight points, each higher than the last.
            reds = [at(cx + x, cy + 24)[0] for x in range(4, 64, 8)]
            greens = [at(cx + 32, cy + y)[1] for y in range(3, 48, 6)]
            check("and they rise where they should - red to the right %s"
                  % reds,
                  all(reds[i] < reds[i + 1] for i in range(len(reds) - 1)) and
                  reds[0] < 40 and reds[-1] > 200)
            check("and green downwards %s" % greens,
                  all(greens[i] < greens[i + 1]
                      for i in range(len(greens) - 1)) and
                  greens[0] < 40 and greens[-1] > 200)

        finally:
            q.close()
    finally:
        stop_http()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
