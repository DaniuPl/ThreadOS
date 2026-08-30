#!/usr/bin/env python3
"""Test dymny ProtonOS: mysz PS/2.

Mysz podaje delty wzgledne, a nie polozenie - to jadro musi je zliczac
i przycinac do rozmiaru ekranu. Start wypada na srodku 320x200, czyli
(160,100), a QEMU przekazuje delty jeden do jednego, wiec kazdy ruch da
sie sprawdzic arytmetycznie zamiast "na oko".

Uruchamiany osobno albo przez scripts/run_tests.py, ktory puszcza
wszystkie grupy rownolegle.
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
