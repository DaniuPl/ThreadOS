#!/usr/bin/env python3
"""Test dymny ProtonOS: konsole wirtualne w jadrze 64-bitowym.

Sprzet ma jeden ekran, a system trzyma ich cztery. Alt+F1..F4 przelacza
obraz, ale kazda konsola ma wlasna tresc, wlasny shell i wlasnych
pisarzy: o tym, na ktorej konsoli pisze program, decyduje ZADANIE,
a nie to, na co akurat patrzymy. Dlatego program uruchomiony na drugiej
konsoli pisze na niej dalej, gdy patrzymy na pierwsza - i to jest
wlasciwa asercja tego testu, mocniejsza niz samo przelaczanie obrazu.

Caly bufor konsol jest w kernel/vga.c, wspolnym z jadrem 32-bitowym.
W trybie dlugim doszla do tego wlasnosc zadania (task_vc) i osobna
kolejka klawiszy na kazda konsole.

Ekran czytamy z bufora obrazu, a nie z pamieci znakow spod 0xB8000:
GRUB, widzac w naglowku jadra zadanie trybu obrazu, i tak daje grafike,
wiec konsola rysuje sie sama czcionka 8x8. Odczyt jest wiec taki, jaki
ma czlowiek - z tego, co naprawde widac.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def main():
    print("--- konsole wirtualne Alt+F1..F4 (jadro 64-bitowe) ---")

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

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=60)

            # Konsola pierwsza: cos, co zostanie na niej na dluzej
            q.type_line("clear")
            q.type_line("uptime")
            time.sleep(1.0)
            first_one = q.wait_fbcon("Uptime:", timeout=10)
            check("pierwsza konsola dziala jak dotad",
                  "Uptime:" in first_one)

            # Alt+F2: druga konsola ma wlasny shell i jest pusta
            q.sendkey("alt-f2")
            time.sleep(0.8)
            second_one = q.screen_fbcon()
            check("Alt+F2 pokazuje druga konsole z wlasnym promptem",
                  "konsola 2" in second_one)
            check("tresc pierwszej konsoli nie przecieka na druga",
                  "Uptime:" not in second_one)

            # Program uruchomiony na drugiej konsoli pisze na niej
            q.type_line("run spin64")
            view = q.wait_fbcon("[spin64]", timeout=20)
            check("program uruchomiony na drugiej konsoli pisze na niej",
                  "[spin64]" in view)

            # Powrot na pierwsza: jej tresc nietknieta, a program
            # z drugiej dalej pisze u siebie, nie tutaj
            q.sendkey("alt-f1")
            time.sleep(1.5)
            returned = q.screen_fbcon()
            check("pierwsza konsola nietknieta po powrocie",
                  "Uptime:" in returned)
            check("program z drugiej konsoli nie pisze po pierwszej",
                  "[spin64]" not in returned)

            # Ctrl+C dotyczy konsoli, na ktorej pada: tu nie ma czego
            # przerywac, a program na drugiej ma dzialac dalej
            q.sendkey("ctrl-c")
            time.sleep(1.0)
            q.sendkey("alt-f2")
            time.sleep(1.0)
            again = q.screen_fbcon()
            check("program na drugiej konsoli przezyl Ctrl+C na pierwszej",
                  "[spin64]" in again)

            # Ctrl+C na jego wlasnej konsoli program konczy. Dowod
            # jest podwojny: shell melduje koniec dziecka, a ekran
            # przestaje sie zmieniac - spin64 pisze co pol sekundy,
            # wiec zywy zostawilby slad.
            q.sendkey("ctrl-c")
            time.sleep(2.0)
            po = q.screen_fbcon()
            check("Ctrl+C na wlasnej konsoli przerywa program",
                  "koniec, kod wyjscia" in po)
            time.sleep(2.0)
            check("przerwany program przestal pisac",
                  q.screen_fbcon() == po)
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
