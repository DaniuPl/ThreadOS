#!/usr/bin/env python3
"""ThreadOS smoke test: the PS/2 mouse.

A mouse gives relative deltas rather than a position - it is the kernel
that has to add them up and clamp them to the screen's size. QEMU passes
the deltas one to one, so every movement can be checked arithmetically
rather than by eye: the desktop draws the pointer's white tip exactly
where the pointer is, and the screen is read there.

A button is a bit in the packet, and a tap can be over before anybody
looks - the desktop looks once a frame. The press is kept until it has
been seen once, so a tap sent down and up in one breath still opens the
menu, and is not seen twice: a second tap closes it again.

The fourth byte - the wheel, which is how a laptop's touchpad reports
two fingers - is asked for with a knock of three sample rates, and the
device answers with who it is. That the asking worked is asked here;
what the wheel then DOES is the 'wheel' group.

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

WHITE = (255, 255, 255)


def main():
    slog = tempfile.mktemp(suffix=".log")
    q = Qemu(KERNEL, INITRD, serial=slog)
    try:
        boot = q.wait_serial("PS/2 keyboard", timeout=90)
        time.sleep(0.5)
        check("the machine negotiated a fourth byte - the wheel",
              "PS/2 keyboard (IRQ 1) and mouse (IRQ 12), wheel: yes"
              in q.serial())
        check("the desktop came up", q.wait_desktop(timeout=60))
        w, h = q.size()

        def tip(x, y):
            """The pointer's tip and the pixel under it are white
            (desktop.c's cursor_bmp), wherever on the background"""
            return (q.wait_pixel(x, y, WHITE, tol=8)
                    and q.wait_pixel(x, y + 1, WHITE, tol=8))

        # Pushed far up and left, the pointer stops in the corner: from
        # there every position is the sum of what was sent
        q.mouse_home()
        q.mouse_move(500, 250)
        check("the pointer is where the deltas add up to: (0,0)+(500,250)",
              tip(500, 250))

        q.mouse_move(-80, -50)
        check("another move, back up and left: (500,250)+(-80,-50) -> "
              "(420,200)", tip(420, 200))
        check("and the old place was painted over with the background",
              q.wait_background(500, 251, tol=12))

        # Past the far edge it stops at the last pixel, rather than
        # wandering off where nothing can be seen: the way back is then
        # counted from the corner
        q.mouse_move(2 * w, 2 * h)
        q.mouse_move(-100, -100)
        check("past the bottom right corner the pointer stops at the edge "
              "(%dx%d)" % (w, h), tip(w - 1 - 100, h - 1 - 100))

        # A tap: down and up a millisecond apart, so both packets come
        # before the desktop's next look. (One message carrying both
        # would be one packet, with the button already up.)
        def tap():
            for down in (True, False):
                q.cmd({"execute": "input-send-event", "arguments": {
                    "events": [{"type": "btn",
                                "data": {"button": "left", "down": down}}]}})

        def entry(name):
            top = menu_y(name) - MENU_ROW // 2
            return q.read_ui(MENU_X0 + 10 + MENU_ICON + 10,
                             ui_font_baseline_in(MENU_PX, top, MENU_ROW),
                             width=100, px=MENU_PX)

        def menu_open():
            # Entries well below the desktop icons, which show through
            # the menu's glass
            return (entry("gmikser"), entry("gcalc")) == ("Mixer",
                                                          "Calculator")

        q.mouse_home()
        q.mouse_goto(*LOGO_XY)
        time.sleep(0.5)
        check("the menu is closed before the tap", not menu_open())
        tap()
        time.sleep(1.0)
        check("a tap between two looks is still seen: the menu opened",
              menu_open())
        tap()
        time.sleep(1.0)
        check("and seen only once: a second tap closes it again",
              not menu_open())
    finally:
        q.close()
        if os.path.exists(slog):
            os.unlink(slog)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
