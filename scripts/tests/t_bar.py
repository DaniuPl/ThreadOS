#!/usr/bin/env python3
"""ProtonOS smoke test: the taskbar - pictures on it, and its shape.

The bar carried names, trimmed to six letters, so 'Settings' read as
'Configi' and two terminals read the same. It carried them because a
window knows its title and nothing else - not what was clicked to start
it.

Now the desktop remembers: when it launches something from an icon it
notes which icon it was, and the window that process opens a moment
later wears the same picture. Nothing else can join the two - the
compositor sees a pid, and the icons are the only place where a program
has a face.

And the bar itself became a matter of taste rather than of fate. Two
settings, kept on the disk like the rest:

  'pasek'  - 100, 75 or 50 percent of the screen. Narrower than the
             screen it is centred, which is what makes it a dock.
  'ikony'  - 0 from the left, 1 in the middle, 2 up against the clock.

What is asserted here is what a person would check: that the bar really
is that wide, that the pictures really move, that two windows of
DIFFERENT programs get different pictures - and, the one that matters
most, that a button is still clickable exactly where it is drawn, in
every one of those arrangements.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, font8x8

TB_H = 18
CLOCK_W = 8 * 8 + 6


def bar_span(at, w, y):
    """Where the bar begins and ends: it is a light strip, and beside
    it (when it is narrower than the screen) there is desktop."""
    xs = [x for x in range(w)
          if at(x, y)[0] > 160 and at(x, y)[1] > 160 and at(x, y)[2] > 160]
    return (min(xs), max(xs)) if xs else (-1, -1)


def buttons(at, w, y, x_from, x_to):
    out, start = [], None
    for x in range(x_from, x_to):
        r, g, b = at(x, y)
        on_bar = r > 160 and g > 160 and b > 160
        if not on_bar and start is None:
            start = x
        elif on_bar and start is not None:
            out.append((start, x - 1))
            start = None
    if start is not None:
        out.append((start, x_to - 1))
    return out


def picture(at, span, ty):
    """The pixels of the picture on a button, as a tuple - so that two
    buttons can be compared without knowing what is drawn on them."""
    a, b = span
    return tuple(at(x, ty + 3 + dy)
                 for dy in range(12) for x in range(a, b + 1))


def read_line(q, ox, oy, cols):
    """One line of text off the screen, through the kernel's own
    font - for reading what a hint says."""
    w, h, at = q.screen_raw()
    font = font8x8()
    counts = {}
    for y in range(8):
        for x in range(ox, ox + cols * 8):
            p = at(x, oy + y)
            counts[p] = counts.get(p, 0) + 1
    bg = max(counts, key=counts.get)

    out = ""
    for c in range(cols):
        pattern = []
        for y in range(8):
            bits = 0
            for x in range(8):
                if at(ox + c * 8 + x, oy + y) != bg:
                    bits |= 1 << x
            pattern.append(bits)
        out += font.get(tuple(pattern), " " if not any(pattern) else "?")
    return out.rstrip()


def open_apps(q, icon_ys):
    for iy in icon_ys:
        q.mouse_goto(24, iy)
        q.mouse_click()
        time.sleep(2.5)
    q.mouse_goto(600, 300)
    time.sleep(1.5)


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")

    print("--- the pictures on the bar ---")
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()
            # The clock (icon 1) and the settings (icon 6): two
            # different programs, so two different pictures
            open_apps(q, [72, 272])

            w, h, at = q.screen_raw()
            ty = h - TB_H
            row = ty + 2
            spans = buttons(at, w, row, 46, w - CLOCK_W - 2)
            check("four windows have buttons (two of the kernel's and two applications)",
                  len(spans) == 4)

            pics = [picture(at, s, ty) for s in spans]
            check("the clock button and the settings button have DIFFERENT pictures",
                  pics[2] != pics[3])
            check("and both have something drawn on them",
                  any(p != pics[2][0] for p in pics[2]) and
                  any(p != pics[3][0] for p in pics[3]))
            # Two windows of the kernel itself, which nobody launched
            # from an icon: they share the plain window mark
            check("the kernel's windows have the same default picture",
                  pics[0] == pics[1])

            # --- the launch menu shows the same faces ----------------
            # A program that looks like three different things - an
            # icon, a button and a word in a menu - is three things to
            # remember. It is one picture in all three places.
            q.mouse_goto(20, ty + 8)
            q.mouse_click()
            time.sleep(1)
            q.mouse_goto(300, 300)          # the pointer off the menu
            time.sleep(1)
            w, h, at = q.screen_raw()
            entry_y = ty - 2 * 16          # the third entry from the top
            marks = sum(1 for dy in range(12) for dx in range(12)
                        if at(8 + dx, entry_y + 2 + dy) == (0, 0, 0))
            check("a menu entry has a picture (%d pixels)" % marks, marks > 8)

            # The entry under the pointer answers: it wears the accent
            q.mouse_goto(60, entry_y + 8)
            time.sleep(1)
            w, h, at = q.screen_raw()
            r, g, b = at(100, entry_y + 8)
            check("the entry under the pointer is highlighted (%d,%d,%d)"
                  % (r, g, b), b > 120 and r < 90 and g < 90)
            others = at(100, entry_y - 16 + 8)
            check("and the other entries are not", not (others[2] > 120 and
                                           others[0] < 90 and others[1] < 90))

            # --- the name comes back when asked for ------------------
            # A picture says WHICH program; it does not say which of
            # two terminals. The name appears over the button the
            # pointer is on, and only then.
            q.mouse_goto(20, ty + 8)       # close the menu
            q.mouse_click()
            time.sleep(1)
            last = spans[-1]               # the settings window's button
            q.mouse_goto((last[0] + last[1]) // 2, ty + 8)
            time.sleep(1.5)
            said = read_line(q, last[0] + 4, ty - 12, 14)
            check("the window's name appears above the button ('%s')" % said,
                  "Settings" in said)

            q.mouse_goto(300, 300)         # the pointer off the bar
            time.sleep(1.5)
            gone = read_line(q, last[0] + 4, ty - 12, 14)
            check("and it goes when the pointer leaves ('%s')" % gone,
                  "Settings" not in gone)
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    # --- the shape of the bar is a setting ---------------------------
    for width, align, name in ((50, 1, "half the screen, icons centred"),
                               (75, 2, "three quarters, icons from the right")):
        print("--- the bar: %s ---" % name)
        disk2 = tempfile.mktemp(suffix=".img")
        with open(disk2, "wb") as f:
            f.truncate(16 * 1024 * 1024)
        try:
            q2 = Qemu(KERNEL, INITRD, disk2, append=TEXT_MODE,
                      serial=tempfile.mktemp(suffix=".log"))
            try:
                q2.wait_for("ProtonOS shell", timeout=40)
                for cmd in ("set taskbar %d" % width, "set icons %d" % align):
                    q2.type_line("sudo " + cmd)
                    time.sleep(1)
                    q2.command("proton", "saved", timeout=30)
                q2.type_line("desktop")
                time.sleep(5)
                q2.mouse_home()
                open_apps(q2, [72, 272])

                w, h, at = q2.screen_raw()
                ty = h - TB_H
                row = ty + 2
                x0, x1 = bar_span(at, w, ty + 1)
                wide = x1 - x0 + 1
                want = w * width // 100
                check("the bar takes %d%% of the screen (%d of %d px)"
                      % (width, wide, w), abs(wide - want) <= 8)
                check("and it is centred (from %d to %d)" % (x0, x1),
                      abs(x0 - (w - want) // 2) <= 6)

                spans = buttons(at, w, row, x0 + 46, x1 - CLOCK_W - 2)
                check("the buttons are on the bar (%d)" % len(spans),
                      len(spans) == 4)
                if align == 1:
                    middle = (spans[0][0] + spans[-1][1]) // 2
                    check("the icons stand in the middle of the free room "
                          "(the middle %d, the bar %d..%d)" % (middle, x0, x1),
                          abs(middle - (x0 + x1) // 2) <= 24)
                else:
                    gap = (x1 - CLOCK_W) - spans[-1][1]
                    check("the icons are pushed up against the clock (a gap of %d px)"
                          % gap, gap <= 12)

                # The one that matters: a button clicks where it is
                # drawn, whatever the settings did to the bar
                last = spans[-1]
                q2.mouse_goto((last[0] + last[1]) // 2, ty + 8)
                time.sleep(0.5)
                q2.mouse_click()
                time.sleep(1.5)
                q2.mouse_goto(600, 300)
                time.sleep(1.5)
                w, h, at = q2.screen_raw()
                spans2 = buttons(at, w, row, x0 + 46, x1 - CLOCK_W - 2)
                a, b = spans2[-1]
                r, g, bl = at((a + b) // 2, ty + 2)
                check("and a click hits the button where it is seen",
                      bl > 120 and r < 90 and g < 90)
            finally:
                q2.close()
        finally:
            if os.path.exists(disk2):
                os.unlink(disk2)

    return summary()


sys.exit(main())
