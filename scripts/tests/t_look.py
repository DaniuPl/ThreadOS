#!/usr/bin/env python3
"""ProtonOS smoke test: how the desktop looks.

Everything on the screen was drawn with one call: fill this rectangle
with that colour. It worked, and it looked like it - a flat background,
flat windows with a black outline, tiles with a word under them. There
was nothing to tell you which window was listening, nothing to say a
window was ABOVE the desktop rather than in it, and nothing on an icon
but its label.

What replaced it is flat and says things with colour rather than with
relief: a surface is one colour, what separates two surfaces is a
single line, and the only thing that shouts is the accent - which is
reserved for the one thing worth shouting about, namely which window is
listening.

Appearance is not a thing to assert as 'nice', so this group asserts
the STRUCTURE of it, which is what the eye actually reads:

  the background is a slope, not a fill, and not a staircase either -
  it is darker at the floor, in steps small enough not to be counted
  a window is one flat colour with one line round it, and there is
  nothing painted underneath it
  which window has the focus can be SEEN: its title bar and its button
  on the taskbar carry the accent colour and nobody else's does, and
  clicking another window moves both
  an icon is a flat tile with a picture on it, and two icons have
  different pictures

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

TB_H = 18
BLUE = (0, 0, 255)


def lum(px):
    return px[0] + px[1] + px[2]


def is_blue(px):
    r, g, b = px
    return b > 120 and r < 90 and g < 90


def main():
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = Qemu(KERNEL, INITRD, serial=slog)
        try:
            check("pulpit wstal", q.wait_desktop(timeout=40))
            q.mouse_home()
            q.mouse_goto(300, 400)       # the cursor off the column we read
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            ty = h - TB_H

            # --- the background is a slope ---------------------------
            top = lum(at(600, 20))
            mid = lum(at(600, 230))
            bot = lum(at(600, ty - 6))
            check("tlo ciemnieje ku dolowi (%d -> %d -> %d)"
                  % (top, mid, bot), top > mid > bot)
            # ...and the steps are small: a staircase is what six bands
            # would have looked like
            biggest = 0
            prev = lum(at(600, 12))
            for y in range(12, ty - 4, 4):
                now = lum(at(600, y))
                if abs(now - prev) > biggest:
                    biggest = abs(now - prev)
                prev = now
            check("i robi to stopniami, ktorych nie widac (najwiekszy %d)"
                  % biggest, biggest <= 24)

            # --- a window is a flat surface with a line round it -----
            # win0 is at (40,60) 120x70. Its body is one colour: read
            # in four places, it has to be the same in all of them -
            # anything else is a gradient, a bevel or a shadow, and
            # there are none of those any more.
            body = [at(60, 100), at(100, 100), at(60, 120), at(140, 120)]
            check("wnetrze okna jest jednym kolorem (%s)" % (body[0],),
                  all(p == body[0] for p in body))
            # And what separates it from the desktop is a single line,
            # darker than the surface and lighter than black
            line = at(40, 100)
            check("a od pulpitu dzieli je jedna linia (%s)" % (line,),
                  lum(line) < lum(body[0]) - 100 and lum(line) > 120)
            # Nothing is painted under the window: the desktop right
            # beside it is the same as the desktop further away
            check("pod oknem nie ma cienia",
                  at(40 + 120 + 2, 60 + 70 + 2) ==
                  at(40 + 120 + 12, 60 + 70 + 2))

            # --- which window is listening can be seen ---------------
            # Nothing has the focus at boot, so no title bar is blue
            # Past the name written on the bar (and short of the
            # buttons at its other end), so that what is read is the
            # bar itself and not a letter of it
            check("na starcie zaden pasek tytulu nie jest niebieski",
                  not is_blue(at(115, 67)) and not is_blue(at(240, 102)))

            q.mouse_goto(80, 64)         # window 0's title bar
            q.mouse_click()
            time.sleep(1)
            q.mouse_goto(600, 300)
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("po kliknieciu okno 0 ma niebieski pasek",
                  is_blue(at(115, 67)))
            check("a okno 1 dalej nie ma", not is_blue(at(240, 102)))

            q.mouse_goto(220, 99)        # window 1's title bar
            q.mouse_click()
            time.sleep(1)
            q.mouse_goto(600, 300)
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("klik w drugie okno przenosi ten niebieski na nie",
                  is_blue(at(240, 102)) and not is_blue(at(115, 67)))

            # --- the taskbar says the same thing with colour ---------
            # Window 1 has the focus, so its button (the second one)
            # wears the accent and the first one does not
            # Buttons are a picture wide now, not a name: 22 pixels
            b1x = 46 + 22 + 10
            b0x = 46 + 10
            check("przycisk okna z ogniskiem ma kolor akcentu",
                  is_blue(at(b1x, ty + 2)))
            check("a przycisk bez ogniska jest szary",
                  not is_blue(at(b0x, ty + 2)))
            # Flat means flat: the button is the same colour at its top
            # and at its bottom, with no edge drawn round it
            check("i przyciski sa plaskie (bez krawedzi)",
                  at(b0x, ty + 2) == at(b0x, ty + 15))

            # --- an icon has a picture on it -------------------------
            # The tile is orange; the picture in the middle of it is not
            def tile(k, y0):
                return [at(8 + 10 + c, y0 + 6 + r)
                        for r in range(12) for c in range(12)]

            app = tile(0, 20)
            clock_icon = tile(1, 60)
            marks = sum(1 for p in app if p == (255, 255, 255))
            check("ikona ma na sobie rysunek (%d bialych pikseli)"
                  % marks, marks > 12)
            check("i dwie ikony maja rozne rysunki", app != clock_icon)
            # The tile itself is flat: a corner of it, away from the
            # picture, is the same colour as the opposite corner
            check("kafelek ikony jest plaski",
                  at(10, 22) == at(8 + 30, 20 + 21))
        finally:
            q.close()
    finally:
        if os.path.exists(slog):
            os.unlink(slog)

    return summary()


sys.exit(main())
