#!/usr/bin/env python3
"""ProtonOS smoke test: the PS/2 mouse.

A mouse gives relative deltas rather than a position - it is the kernel
that has to add them up and clamp them to the screen's size. The start
falls in the middle of 320x200, that is (160,100), and QEMU passes the
deltas one to one, so every movement can be checked arithmetically
rather than by eye.

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
        q.wait_for("ProtonOS shell", timeout=40)

        q.mouse_move(40, 30)
        q.type_line("mouse")
        check("mysz reaguje na ruch (160,100)+(40,30) -> (200,130)",
              "mysz: x=200 y=130" in q.wait_for("mysz: x=200 y=130"))

        q.mouse_move(-80, -50)
        q.mouse_button(True)
        q.type_line("mouse")
        check("mysz: kolejny ruch i wcisniety LPM (120,80 b1)",
              "mysz: x=120 y=80 przyciski=1"
              in q.wait_for("mysz: x=120 y=80 przyciski=1"))
        q.mouse_button(False)
    finally:
        q.close()

    return summary()


if __name__ == "__main__":
    sys.exit(main())
