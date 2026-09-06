#!/usr/bin/env python3
"""ProtonOS smoke test: virtual consoles (Alt+F1..F4).

Alt+F1..F4 switches the screen, but every console has its own content,
its own shell and its own "own" writers: a program started on the second
keeps writing on it while we look at the first.

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


def main():
    # --- Virtual consoles ------------------------------------------
    # Alt+F1..F4 switches the screen, but every console has its own
    # content, its own shell and its own "own" writers: a program started
    # on the second keeps writing on it while we look at the first.
    print("--- virtual consoles (Alt+F1..F4) ---")
    q17 = Qemu(KERNEL, INITRD, append=TEXT_MODE)
    try:
        q17.wait_for("ProtonOS shell", timeout=30)
        q17.type_line("clear")
        q17.type_line("uptime")
        first = q17.wait_for("timer ticks", timeout=20)
        check("the first console works as before", "uptime:" in first)

        q17.sendkey("alt-f2")
        time.sleep(0.6)
        second = q17.screen()
        check("Alt+F2 shows the second console with a prompt of its own",
              "console 2" in second)
        check("what is on the first console does not leak onto the second",
              "timer ticks" not in second)

        q17.type_line("run spin")
        view = q17.wait_for("[spin] tick", timeout=20)
        check("a program started on the second console writes to it",
              "[spin] tick" in view)

        q17.sendkey("alt-f1")
        time.sleep(1.5)
        back = q17.screen()
        check("the first console is untouched when we come back",
              "timer ticks" in back)
        check("a program on the second console does not write over the first",
              "[spin] tick" not in back)

        q17.sendkey("alt-f2")
        time.sleep(0.5)
        before = q17.screen().count("[spin] tick")
        time.sleep(1.5)
        after = q17.screen().count("[spin] tick")
        check("the program wrote to its own console even while it was not shown"
              f" shows ({before} -> {after})",
              after != before or before >= 8)
        q17.type_line("kill spin")
        view = q17.wait_for("killed (code -9)", timeout=20)
        check("kill from a console ends that console's program",
              "killed (code -9)" in view)
    finally:
        q17.close()

    return summary()


if __name__ == "__main__":
    sys.exit(main())
