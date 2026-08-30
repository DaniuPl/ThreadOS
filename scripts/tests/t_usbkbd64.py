#!/usr/bin/env python3
"""Test dymny ProtonOS: klawiatura USB w jadrze 64-bitowym.

Maszyna nowsza niz mniej wiecej 2010 rok nie ma juz zlacza PS/2.
Ta grupa pilnuje starszej magistrali - UHCI. Nowsza, xHCI, ma wlasna
(t_xhci.py); wazne jest, ze WYZEJ nie widac miedzy nimi roznicy.

Sterownik UHCI i klasa HID w protokole rozruchowym to te same pliki,
co w jadrze 32-bitowym - w trybie dlugim trzeba bylo tylko jawnie
rzutowac adresy struktur, bo kontroler adresuje pamiec 32 bitami,
a wskaznik ma tu osiem bajtow.

QEMU dostaje 'usb-kbd' i kieruje do niego klawisze, wiec samo to, ze
polecenie sie wykonuje, jest dowodem, ze przeszly cala droge: przez
kolejke deskryptorow kontrolera, raport HID i kolejke konsoli.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def main():
    print("--- klawiatura USB (HID) w jadrze 64-bitowym ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    try:
        check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, usbkbd=True,
                 qemu="qemu-system-x86_64")
        try:
            start = q.wait_serial("Klawiatura USB", timeout=60)
            check("klawiatura USB rozpoznana na magistrali",
                  re.search(r"Klawiatura USB \(HID, UHCI\) na porcie \d+",
                            start) is not None)

            q.wait_serial("proton64>", timeout=30)
            q.type_line("uptime")
            out_text = q.wait_serial("Uptime:", timeout=20)
            check("polecenie wpisane z klawiatury USB wykonuje sie",
                  "tyknac po" in out_text)

            # Wielka litera powstaje z modyfikatora w raporcie HID.
            # Klawiature odpytujemy piecdziesiat razy na sekunde,
            # a QEMU trzyma klawisz okolo stu milisekund - przy zbyt
            # szybkim nadawaniu press i release sasiednich klawiszy
            # zachodza na siebie. Stad wolniejsze tempo i druga proba.
            def type_shifted():
                for ch in "echo":
                    q.sendkey(ch)
                    time.sleep(0.15)
                for k in ("spc", "shift-a", "ret"):
                    q.sendkey(k)
                    time.sleep(0.25)

            big_one = False
            for _ in range(3):
                before = len(q.serial())
                type_shifted()
                time.sleep(1.0)
                if "echo A" in q.serial()[before:]:
                    big_one = True
                    break
            check("modyfikator Shift z raportu HID daje wielka litere", big_one)

            # Alt+F2 z klawiatury USB przelacza konsole - to inna droga
            # w jadrze niz zwykly znak (przelaczenie, nie kolejka)
            q.sendkey("alt-f2")
            time.sleep(1.0)
            second_one = q.screen_fbcon()
            check("Alt+F2 z klawiatury USB przelacza konsole",
                  "konsola 2" in second_one)
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
