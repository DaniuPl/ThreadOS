#!/usr/bin/env python3
"""ProtonOS smoke test: ACPI - powering off.

The most honest proof one can have: the machine is simply to disappear.
The emulator ends, rather than pretending something happened.

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
    print("--- ACPI (wylaczanie zasilania) ---")
    q20 = Qemu(KERNEL, INITRD, append=TEXT_MODE)
    try:
        boot = q20.wait_for("ACPI:", timeout=30)
        check("tablice ACPI znalezione przy starcie",
              re.search(r"ACPI: tablice od '\S+", boot) is not None)
        q20.wait_for("ProtonOS shell", timeout=20)
        q20.type_line("help")
        check("'poweroff' jest w spisie komend",
              "poweroff" in q20.wait_for("Dostepne komendy", timeout=20))
        q20.type_line("poweroff")
        check("'poweroff' faktycznie gasi maszyne", q20.wait_exit(timeout=20))
    finally:
        q20.close()

    return summary()


if __name__ == "__main__":
    sys.exit(main())
