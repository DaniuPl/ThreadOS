#!/usr/bin/env python3
"""Test dymny ProtonOS: konsole wirtualne (Alt+F1..F4).

Alt+F1..F4 przelacza ekran, ale kazda konsola ma wlasna tresc, wlasny
shell i wlasnych "swoich" pisarzy: program uruchomiony na drugiej pisze
na niej dalej, kiedy patrzymy na pierwsza.

Uruchamiany osobno albo przez scripts/run_tests.py, ktory puszcza
wszystkie grupy rownolegle.
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
    # --- Konsole wirtualne -----------------------------------------
    # Alt+F1..F4 przelacza ekran, ale kazda konsola ma wlasna tresc,
    # wlasny shell i wlasnych "swoich" pisarzy: program uruchomiony na
    # drugiej pisze na niej dalej, kiedy patrzymy na pierwsza.
    print("--- konsole wirtualne (Alt+F1..F4) ---")
    q17 = Qemu(KERNEL, INITRD, append=TEXT_MODE)
    try:
        q17.wait_for("ProtonOS shell", timeout=30)
        q17.type_line("clear")
        q17.type_line("uptime")
        first = q17.wait_for("tyknac timera", timeout=20)
        check("pierwsza konsola dziala jak dotad", "uptime:" in first)

        q17.sendkey("alt-f2")
        time.sleep(0.6)
        second = q17.screen()
        check("Alt+F2 pokazuje druga konsole z wlasnym promptem",
              "konsola 2" in second)
        check("tresc pierwszej konsoli nie przecieka na druga",
              "tyknac timera" not in second)

        q17.type_line("run spin")
        view = q17.wait_for("[spin] tik", timeout=20)
        check("program uruchomiony na drugiej konsoli pisze na niej",
              "[spin] tik" in view)

        q17.sendkey("alt-f1")
        time.sleep(1.5)
        back = q17.screen()
        check("pierwsza konsola nietknieta po powrocie",
              "tyknac timera" in back)
        check("program z drugiej konsoli nie pisze po pierwszej",
              "[spin] tik" not in back)

        q17.sendkey("alt-f2")
        time.sleep(0.5)
        before = q17.screen().count("[spin] tik")
        time.sleep(1.5)
        after = q17.screen().count("[spin] tik")
        check("program pisal na swoja konsole takze wtedy, gdy jej nie bylo"
              f" widac ({before} -> {after})",
              after != before or before >= 8)
        q17.type_line("kill spin")
        view = q17.wait_for("zabity (kod -9)", timeout=20)
        check("kill z wlasnej konsoli konczy jej program",
              "zabity (kod -9)" in view)
    finally:
        q17.close()

    return summary()


if __name__ == "__main__":
    sys.exit(main())
