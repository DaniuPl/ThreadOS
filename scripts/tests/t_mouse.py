#!/usr/bin/env python3
"""ThreadOS smoke test: the PS/2 mouse.

A mouse gives relative deltas rather than a position - it is the kernel
that has to add them up and clamp them to the screen's size. The start
falls in the middle of 320x200, that is (160,100), whatever mode the
console is in (kernel/mouse.c), and QEMU passes the deltas one to one,
so every movement can be checked arithmetically rather than by eye.
Pushed far past the top left corner the pointer has to stop at (0,0):
that is the clamp at work.

The fourth byte - the wheel, which is how a laptop's touchpad reports
two fingers - is asked for with a knock of three sample rates, and the
device answers with who it is. That the asking worked is asked here;
what the wheel then DOES is the 'wheel' group.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary


def main():
    q = Qemu(KERNEL, INITRD, append=TEXT_MODE)
    try:
        q.wait_for("thread64>", timeout=40)

        # Before anything moved: the start, which every sum below
        # begins from
        q.type_line("mouse")
        check("the pointer starts at (160,100)",
              "mouse: x=160 y=100 buttons=0"
              in q.wait_for("mouse: x=160 y=100 buttons=0"))

        q.mouse_move(40, 30)
        q.type_line("mouse")
        check("the mouse answers movement (160,100)+(40,30) -> (200,130)",
              "mouse: x=200 y=130" in q.wait_for("mouse: x=200 y=130"))

        q.mouse_move(-80, -50)
        q.mouse_button(True)
        q.type_line("mouse")
        check("the mouse: another move with the left button down (120,80 b1)",
              "mouse: x=120 y=80 buttons=1"
              in q.wait_for("mouse: x=120 y=80 buttons=1"))
        q.mouse_button(False)

        # A click quicker than anybody looks: down and up before the
        # next question. The press is kept until it has been seen
        # once - so the first look says the button went down, and the
        # second that it is up. (The desktop looks once a frame; a
        # tap shorter than a frame used to go missing.)
        q.mouse_move(10, 10)
        q.mouse_button(True)
        q.mouse_button(False)
        q.type_line("mouse")
        check("a click between two looks is still seen once (130,90 b1)",
              "mouse: x=130 y=90 buttons=1"
              in q.wait_for("mouse: x=130 y=90 buttons=1"))
        q.type_line("mouse")
        check("and only once - the next look sees the button up",
              "mouse: x=130 y=90 buttons=0"
              in q.wait_for("mouse: x=130 y=90 buttons=0"))

        # Further than any screen is wide or tall: the sum goes below
        # zero on both axes, and only the clamp brings it back to the
        # corner. (It comes last: the moves above are sums from the
        # start.)
        q.mouse_move(-800, -600)
        q.type_line("mouse")
        check("pushed past the top left corner the pointer stops at (0,0)",
              "mouse: x=0 y=0 buttons=0"
              in q.wait_for("mouse: x=0 y=0 buttons=0"))

        # The wheel. A laptop's touchpad reports two fingers sliding
        # through it, and the fourth byte that carries it has to be
        # asked for - QEMU's mouse sends it only to a guest that did.
        # Only what this 'mouse' said counts: the earlier answers are
        # still on the port.
        said = q.command("mouse", "wheel=yes", timeout=20)
        line = next((ln.strip() for ln in said.splitlines()
                     if ln.strip().startswith("mouse: x=")), "")
        check("the machine negotiated a fourth byte - the wheel ('%s')"
              % line[:50], line.endswith("wheel=yes"))
    finally:
        q.close()

    return summary()


if __name__ == "__main__":
    sys.exit(main())
