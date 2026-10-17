#!/usr/bin/env python3
"""ThreadOS smoke test: the PS/2 mouse.

A mouse gives relative deltas rather than a position - it is the kernel
that has to add them up and clamp them to the screen's size. The start
falls in the middle of 320x200, that is (160,100), and QEMU passes the
deltas one to one, so every movement can be checked arithmetically
rather than by eye.

The fourth byte - the wheel, which is how a laptop's touchpad reports
two fingers - is asked for with a knock of three sample rates, and the
device answers with who it is. That the asking worked is asked here;
what the wheel then DOES is the 'kolko' group.

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
        q.wait_for("ThreadOS shell", timeout=40)

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

        # The wheel. A laptop's touchpad reports two fingers sliding
        # through it, and the fourth byte that carries it has to be
        # asked for - QEMU's mouse sends it only to a guest that did.
        q.type_line("mouse")
        said = q.wait_for("wheel=")
        check("the machine negotiated a fourth byte - the wheel ('%s')"
              % said.strip().splitlines()[-1][:50],
              "wheel=yes" in said)
    finally:
        q.close()

    return summary()


if __name__ == "__main__":
    sys.exit(main())
