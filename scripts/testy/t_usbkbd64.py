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
from ptest import check, wynik, zrob_iso64, brak64


def main():
    print("--- klawiatura USB (HID) w jadrze 64-bitowym ---")

    powod = brak64()
    if powod:
        print("POMINIETE: " + powod)
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    try:
        check("plyta z jadrem 64-bitowym buduje sie", zrob_iso64(d, iso))
        if not os.path.exists(iso):
            return wynik()

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, usbkbd=True,
                 qemu="qemu-system-x86_64")
        try:
            start = q.wait_serial("Klawiatura USB", timeout=60)
            check("klawiatura USB rozpoznana na magistrali",
                  re.search(r"Klawiatura USB \(HID, UHCI\) na porcie \d+",
                            start) is not None)

            q.wait_serial("proton64>", timeout=30)
            q.type_line("uptime")
            wyjscie = q.wait_serial("Uptime:", timeout=20)
            check("polecenie wpisane z klawiatury USB wykonuje sie",
                  "tyknac po" in wyjscie)

            # Wielka litera powstaje z modyfikatora w raporcie HID.
            # Klawiature odpytujemy piecdziesiat razy na sekunde,
            # a QEMU trzyma klawisz okolo stu milisekund - przy zbyt
            # szybkim nadawaniu press i release sasiednich klawiszy
            # zachodza na siebie. Stad wolniejsze tempo i druga proba.
            def wpisz_z_shiftem():
                for ch in "echo":
                    q.sendkey(ch)
                    time.sleep(0.15)
                for k in ("spc", "shift-a", "ret"):
                    q.sendkey(k)
                    time.sleep(0.25)

            duza = False
            for _ in range(3):
                przed = len(q.serial())
                wpisz_z_shiftem()
                time.sleep(1.0)
                if "echo A" in q.serial()[przed:]:
                    duza = True
                    break
            check("modyfikator Shift z raportu HID daje wielka litere", duza)

            # Alt+F2 z klawiatury USB przelacza konsole - to inna droga
            # w jadrze niz zwykly znak (przelaczenie, nie kolejka)
            q.sendkey("alt-f2")
            time.sleep(1.0)
            druga = q.screen_fbcon()
            check("Alt+F2 z klawiatury USB przelacza konsole",
                  "konsola 2" in druga)
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog):
            if os.path.exists(f):
                os.unlink(f)

    return wynik()


if __name__ == "__main__":
    sys.exit(main())
