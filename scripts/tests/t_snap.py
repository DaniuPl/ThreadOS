#!/usr/bin/env python3
"""ProtonOS smoke test: snapping a window to an edge of the screen.

Two windows side by side used to mean two drags and two pulls of a
corner, done by eye, and the eye is not good at halves. Now a window
pulled against the left edge takes the left half, against the right
edge the right half, and against the top the whole screen - which is
the fastest way there is to put two things next to each other, and
costs nothing to learn, because the window goes where it was pushed.

While the button is still down the place the window WOULD take is
drawn as an outline in the accent colour. A window that jumps
somewhere on release, unannounced, is a window that got away: the
outline is the question and letting go is the answer.

The size the window had is kept the way maximising keeps it, so the
[] button gives it back - a window held at half the screen has no more
lost its own size than a maximised one has.

The assertions are about all of that: that the outline appears only at
an edge, that the window really ends up half the screen wide (grey on
its side, desktop on the other), that the application inside it
redraws itself at the new size rather than leaving the old picture,
that the other edge and the top do what they say, and that the []
button gives the window its own size back.

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
TITLE_H = 10
CLOSE_SZ = 8

# The cascade: slot 2 is the first application window, at (94,54);
# gsettings asks for a 360x210 content buffer.
WX, WY, WW, WH = 94, 54, 362, 222


def near(px, v):
    return all(abs(c - v) <= 6 for c in px)


def is_win(px):
    return near(px, 227)            # COL_WIN, a window's background


def is_accent(px):
    r, g, b = px                    # COL_TITLE, the accent blue
    return b > 120 and r < 90 and g < 90


def title_run(at, w, top=60):
    """The widest run of the accent colour in the top strip of the
    screen: the title bar of the window that has the focus. Its width
    is the window's width, which is the thing a snap changes."""
    best = (0, 0, 0)
    for y in range(0, top):
        x = 0
        while x < w:
            if is_accent(at(x, y)):
                s = x
                while x < w and is_accent(at(x, y)):
                    x += 1
                if x - s > best[0]:
                    best = (x - s, s, y)
            else:
                x += 1
    return best


def drag(q, x0, y0, x1, y1, hold=None):
    """Pull the window by its title bar from one place to another. If
    'hold' is given it is called with the button still down - which is
    the only moment the outline exists."""
    q.mouse_goto(x0, y0)
    time.sleep(0.4)
    q.mouse_button(True)
    time.sleep(0.4)
    q.mouse_goto(x1, y1)
    time.sleep(0.5)
    q.mouse_goto(x1, y1)
    time.sleep(0.5)
    if hold:
        hold()
    q.mouse_button(False)
    time.sleep(1.5)


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")

    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()

            q.mouse_goto(24, 272)              # the "Config" icon
            q.mouse_click()
            time.sleep(4)
            q.mouse_goto(600, 400)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            half = w // 2
            deck = h - TB_H                    # the room a window may have
            check("the settings stand where they always do",
                  is_win(at(WX + 300, WY + 150)))

            # --- a drag that is NOT at an edge shows nothing ------------
            seen = []
            drag(q, WX + 100, WY + 4, 300, 200,
                 hold=lambda: seen.append(q.screen_raw()))
            _, _, mid = seen[0]
            check("there is no outline in the middle of the screen",
                  not is_accent(mid(half - 2, deck - 40)) and
                  not is_accent(mid(4, deck - 40)))

            # --- the left edge ------------------------------------------
            # The window is now at (200,196) - the drag above moved it
            # by (106,4), and that is where its title bar is
            seen = []
            drag(q, 306, 200, 4, 200,
                 hold=lambda: seen.append(q.screen_raw()))
            _, _, at2 = seen[0]
            # The outline stands where the window would: down the middle
            # of the screen and along its bottom, over the window that is
            # being dragged (it is drawn last, so it is seen)
            check("at the left edge the outline of the left half shows",
                  is_accent(at2(half - 2, 300)) and
                  is_accent(at2(1, 300)))
            check("and the outline ends above the taskbar",
                  is_accent(at2(200, deck - 1)) and
                  not is_accent(at2(200, deck + 6)))

            w, h, at = q.screen_raw()
            check("once dropped the window takes the left half",
                  is_win(at(half - 10, 300)) and not is_win(at(half + 10, 300)))
            check("from the top to the taskbar",
                  is_accent(at(half - 40, 5)) and is_win(at(100, deck - 4)))
            # The program was told its new size and drew itself in it;
            # a buffer that was not remade would leave the old picture
            # or rubbish down there
            check("the program redrew itself in half the screen",
                  is_win(at(20, deck - 20)))

            # --- the right edge -----------------------------------------
            drag(q, 150, 4, w - 5, 200)
            w, h, at = q.screen_raw()
            check("a window dragged right takes the right half",
                  is_win(at(half + 10, 300)) and
                  not is_win(at(half - 10, 300)))
            check("and it reaches the right edge",
                  is_win(at(w - 3, 300)))

            # --- the top: the whole screen ------------------------------
            drag(q, half + 150, 4, 320, 20)
            w, h, at = q.screen_raw()
            check("a window dragged to the top takes the whole screen",
                  is_win(at(20, 300)) and is_win(at(w - 3, 300)))
            check("but it does not run over the taskbar",
                  not is_win(at(320, h - 4)))

            # --- and [] gives it its own size back ----------------------
            q.mouse_goto(w - 3 * CLOSE_SZ - 3 + 4, 6)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(2)
            q.mouse_goto(600, 430)
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("the [] button gives the window its own size back",
                  not is_win(at(w - 3, 300)) and not is_win(at(20, 30)))
            # Its own size is 362 wide - the size it asked for. Half
            # the screen is 320 and the whole of it 640, so this one
            # number tells the three apart.
            wide, x0, ty = title_run(at, w)
            check("and it is its own width (%d px, not half "
                  "nor the whole screen)" % wide, abs(wide - (WW - 2)) <= 4)
            # After four drags, three snaps and a restore it is still a
            # window: a lit title bar with its own body under it
            check("the window survived four drags and still has the focus",
                  is_win(at(x0 + 20, ty + TITLE_H + 40)))

            # --- what all of it is for: two windows side by side --------
            # The settings to the right, the terminal to the left. The
            # terminal's screen is BLACK and a window's background is
            # grey, so one look tells which half holds which - and that
            # they do not overlap.
            drag(q, x0 + 40, ty + 4, w - 5, 200)
            q.mouse_goto(24, 192)              # the "Term" icon
            q.mouse_click()
            time.sleep(4)
            drag(q, 206, 70, 4, 200)
            q.mouse_goto(320, 440)
            time.sleep(1.5)
            w, h, at = q.screen_raw()

            dark = near(at(150, deck - 30), 0)
            check("the terminal takes the left half (a black screen)", dark)
            check("and the settings the right (a grey background)",
                  is_win(at(half + 100, 300)))
            check("and they do not overlap - the border is in the middle",
                  near(at(half - 10, deck - 30), 0) and
                  is_win(at(half + 10, 300)))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
