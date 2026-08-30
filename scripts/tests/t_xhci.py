#!/usr/bin/env python3
"""Test dymny ProtonOS: klawiatura USB na kontrolerze xHCI.

xHCI to trzeci i ostatni kontroler USB - jedyny, jaki maja nowsze
plyty. Rozni sie od UHCI nie szczegolami, tylko podzialem pracy: tam
sterownik ukladal deskryptory przesylek w liscie ramek i sam nadawal
urzadzeniom adresy, tu wklada polecenia do pierscienia, a odpowiedzi
odbiera drugim pierscieniem - zdarzen.

Sprawdzamy dwie rzeczy naraz:

  1. ze sterownik doprowadza klawiature do rozmowy - reset portu,
     przydzial slotu, nadanie adresu, opis konfiguracji, protokol
     rozruchowy, a potem raporty HID lecace pierscieniem przesylek;
  2. ze WYZEJ nie widac zadnej roznicy. Ta sama grupa sprawdzen, co
     przy UHCI (t_usbkbd64.py), na tej samej klawiaturze i tym samym
     kodzie HID - inna jest tylko magistrala pod spodem. Warstwa
     usbhc.c ma to ukryc i wlasnie to tu mierzymy.

Robimy to w obu jadrach: kontroler adresuje pamiec 64 bitami, wiec
w trybie 32-bitowym gorne polowki adresow sa zerami, a rejestry
64-bitowe zapisuje sie dwoma polowkami. Jesli cos w tym jest nie tak,
tylko jedno z jader ruszy.

Uruchamiany osobno albo przez scripts/run_testy.py, ktory puszcza
wszystkie grupy rownolegle.
"""

import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def type_text(q, text, delay=0.12):
    for ch in text:
        q.sendkey(ch)
        time.sleep(delay)
    q.sendkey("ret")
    time.sleep(0.25)


def main():
    print("--- klawiatura USB na xHCI (jadro 64-bitowe) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
    else:
        d = tempfile.mkdtemp()
        iso = tempfile.mktemp(suffix=".iso")
        slog = tempfile.mktemp(suffix=".log")
        try:
            check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
            if not os.path.exists(iso):
                return summary()

            q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, usbkbd="xhci",
                     qemu="qemu-system-x86_64")
            try:
                start = q.wait_serial("Klawiatura USB", timeout=60)
                # Warstwa wyboru sama mowi, przez co gada - i ma tu
                # powiedziec xHCI, bo UHCI na tej maszynie nie ma
                check("klawiatura znaleziona na kontrolerze xHCI",
                      re.search(r"Klawiatura USB \(HID, xHCI\) na porcie \d+",
                                start) is not None)

                q.wait_serial("proton64>", timeout=30)
                time.sleep(0.5)
                before = len(q.serial())
                type_text(q, "uptime")
                time.sleep(1.5)
                po = q.serial()[before:]
                check("polecenie wpisane przez xHCI wykonuje sie",
                      "Uptime:" in po)

                # Wielka litera powstaje z modyfikatora w raporcie HID -
                # to inna droga przez raport niz zwykly kod klawisza.
                # QEMU trzyma klawisz okolo stu milisekund, wiec przy
                # zbyt szybkim nadawaniu sasiednie klawisze zachodza na
                # siebie; stad wolniejsze tempo i druga proba.
                big_one = False
                for _ in range(3):
                    before = len(q.serial())
                    for ch in "echo":
                        q.sendkey(ch)
                        time.sleep(0.15)
                    for k in ("spc", "shift-a", "ret"):
                        q.sendkey(k)
                        time.sleep(0.25)
                    time.sleep(1.0)
                    if "echo A" in q.serial()[before:]:
                        big_one = True
                        break
                check("modyfikator Shift z raportu HID daje wielka litere",
                      big_one)

                # Alt+F2 to inna droga w jadrze niz zwykly znak
                # (przelaczenie konsoli, a nie kolejka wejscia)
                q.sendkey("alt-f2")
                time.sleep(1.0)
                check("Alt+F2 z klawiatury na xHCI przelacza konsole",
                      "konsola 2" in q.screen_fbcon())
            finally:
                q.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)
            if os.path.exists(iso):
                os.unlink(iso)
            if os.path.exists(slog):
                os.unlink(slog)

    # --- To samo w jadrze 32-bitowym ---
    print("--- klawiatura USB na xHCI (jadro 32-bitowe) ---")
    slog32 = tempfile.mktemp(suffix=".log")
    try:
        q3 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog32,
                  usbkbd="xhci")
        try:
            start = q3.wait_serial("Klawiatura USB", timeout=60)
            check("klawiatura znaleziona na kontrolerze xHCI (32-bit)",
                  re.search(r"Klawiatura USB \(HID, xHCI\) na porcie \d+",
                            start) is not None)
            check("kontroler nie zglosil sie jako UHCI",
                  "HID, UHCI" not in start)

            q3.wait_serial("ProtonOS shell", timeout=40)
            time.sleep(1.0)
            before = len(q3.serial())
            type_text(q3, "uptime")
            time.sleep(1.5)
            po = q3.serial()[before:]
            check("polecenie wpisane przez xHCI wykonuje sie (32-bit)",
                  "uptime:" in po and "tyknac" in po)

            # Kilka klawiszy po kolei: raporty musza isc jeden za drugim
            # przez ten sam pierscien przesylek, a nie tylko pierwszy
            before = len(q3.serial())
            type_text(q3, "help")
            time.sleep(1.5)
            help_text = q3.serial()[before:]
            check("cale slowo dochodzi znak po znaku (32-bit)",
                  "help" in help_text and "Dostepne komendy" in help_text)
        finally:
            q3.close()
    finally:
        if os.path.exists(slog32):
            os.unlink(slog32)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
