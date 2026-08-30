#!/usr/bin/env python3
"""Test dymny ProtonOS: klawiatura USB (HID).

Kontroler USB, rozpoznanie urzadzenia i protokol rozruchowy HID.
QEMU kieruje klawisze do urzadzenia USB, wiec to, ze komenda w ogole
sie wykonuje, jest dowodem, ze przeszly cala te droge.

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
    # --- Klawiatura USB --------------------------------------------
    # Kontroler USB, rozpoznanie urzadzenia, protokol rozruchowy HID.
    # QEMU kieruje klawisze do urzadzenia USB, wiec to, ze komenda
    # w ogole sie wykonuje, jest dowodem, ze przeszly ta droga.
    print("--- klawiatura USB (HID) ---")
    udisk = tempfile.mktemp(suffix=".img")
    with open(udisk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    q19 = Qemu(KERNEL, INITRD, udisk, append=TEXT_MODE, usbkbd=True)
    try:
        boot = q19.wait_for("Klawiatura USB", timeout=30)
        check("klawiatura USB rozpoznana na magistrali",
              re.search(r"Klawiatura USB \(HID, UHCI\) na porcie \d+", boot)
              is not None)
        q19.wait_for("ProtonOS shell", timeout=20)
        q19.type_line("clear")
        q19.type_line("uptime")
        out = q19.wait_for("tyknac timera", timeout=20)
        check("komenda wpisana z klawiatury USB wykonuje sie",
              "uptime:" in out)
        # Wielka litera powstaje z modyfikatora w raporcie HID -
        # 'type_line' umie tylko male, wiec skladamy klawisze recznie.
        # Klawiature odpytujemy piecdziesiat razy na sekunde, a QEMU
        # trzyma klawisz okolo stu milisekund - przy zbyt szybkim
        # nadawaniu press i release sasiednich klawiszy zachodza na
        # siebie. Stad wolniejsze tempo i druga proba.
        def type_shift_a():
            for ch in "write":
                q19.sendkey(ch)
                time.sleep(0.15)
            for k in ("spc", "shift-a", "spc", "x", "ret"):
                q19.sendkey(k)
                time.sleep(0.25)

        saved = ""
        for attempt in range(3):
            type_shift_a()
            try:
                saved = q19.wait_for("zapisano", timeout=10)
                if "do 'A'" in saved:
                    break
            except RuntimeError:
                pass
            q19.type_line("clear")
        check("modyfikator Shift z raportu HID daje wielkie litery",
              "do 'A'" in saved)
    finally:
        q19.close()
        if os.path.exists(udisk):
            os.unlink(udisk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
