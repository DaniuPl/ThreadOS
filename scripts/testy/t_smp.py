#!/usr/bin/env python3
"""Test dymny ProtonOS: wykrycie i wybudzenie pozostalych procesorow.

Po wlaczeniu zasilania pracuje jeden rdzen - ten, ktory firmware
wybral na rozruchowy. Reszta stoi i czeka na sygnal, i to jest decyzja
sprzetu, nie systemu: procesor obudzony sygnalem SIPI zaczyna w trybie
rzeczywistym, szesnastobitowo, pod adresem (wektor << 12) - dokladnie
tak, jak zaczynal pecet czterdziesci lat temu.

To jest pierwszy krok, nie caly SMP. Obudzony rdzen przechodzi wlasna
droga do trybu chronionego, zglasza sie i staje. Dawanie mu pracy
wymaga schedulera per rdzen i zamkow w kazdej strukturze jadra, a te
pisane byly dla jednego rdzenia i cicho na to licza.

Dlatego test sprawdza dwie rzeczy naraz: ze rdzenie sie zglaszaja
ORAZ ze system po ich obudzeniu dziala dokladnie tak samo, jak
dzialal. Samo "widze cztery procesory" nie byloby nic warte, gdyby
przy okazji cos w systemie pekalo.
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
    print("--- pozostale procesory (INIT-SIPI) ---")

    # --- Cztery rdzenie ---
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog, cpus=4)
    try:
        start = q.wait_for("Procesory:", timeout=60)
        m = re.search(r"Procesory: (\d+) \(obudzonych (\d+)", start)
        check("system wylicza wszystkie procesory z tablicy ACPI",
              m is not None and int(m.group(1)) == 4)
        check("trzy pozostale rdzenie zglosily sie po INIT-SIPI",
              m is not None and int(m.group(2)) == 3)

        q.wait_for("ProtonOS shell", timeout=30)
        q.wait_for("[siec] DHCP", timeout=30)

        # System po obudzeniu rdzeni ma dzialac jak dotad
        up1 = q.polecenie("uptime", "tyknac timera")
        check("timer tyka po obudzeniu pozostalych rdzeni",
              "tyknac timera" in up1)

        proc = q.polecenie("run hello", "zakonczyl sie", timeout=40)
        check("proces w ring 3 dziala jak dotad",
              "[hello] czesc, jestem procesem ELF" in proc)

        pliki = q.polecenie("write smptest wielordzeniowo", "zapisano")
        check("dysk dziala jak dotad", "zapisano" in pliki)
        odczyt = q.polecenie("cat smptest", "wielordzeniowo")
        check("i oddaje to, co zapisano", "wielordzeniowo" in odczyt)

        # Liczniki watkow rosna dalej - scheduler nie stanal
        def licznik():
            m = re.findall(r"watek-A: (\d+)", q.screen())
            return int(m[-1]) if m else None

        a1 = licznik()
        time.sleep(1.5)
        a2 = licznik()
        check("scheduler dziala dalej (%s -> %s)" % (a1, a2),
              a1 is not None and a2 is not None and a2 > a1)
    finally:
        q.close()
        for f in (slog, disk):
            if os.path.exists(f):
                os.unlink(f)

    # --- Tryb dlugi ---
    # Rdzen wtorny startuje w trybie rzeczywistym takze wtedy, gdy
    # reszta systemu pracuje od dawna 64-bitowo: musi przejsc sam cala
    # droge, szesnascie bitow -> trzydziesci dwa -> szescdziesiat
    # cztery. Tablice stron bierze gotowe, te same, co jadro.
    print("--- pozostale procesory w trybie dlugim ---")
    powod = brak64()
    if powod:
        print("POMINIETE: " + powod)
    else:
        d = tempfile.mkdtemp()
        iso = tempfile.mktemp(suffix=".iso")
        slog64 = tempfile.mktemp(suffix=".log")
        try:
            check("plyta z jadrem 64-bitowym buduje sie", zrob_iso64(d, iso))
            if os.path.exists(iso):
                q64 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog64, cpus=4,
                           qemu="qemu-system-x86_64")
                try:
                    st = q64.wait_serial("Procesory:", timeout=90)
                    m = re.search(r"Procesory: (\d+) \(obudzonych (\d+)", st)
                    check("jadro 64-bitowe wylicza wszystkie procesory",
                          m is not None and int(m.group(1)) == 4)
                    check("rdzenie wtorne same dochodza do trybu dlugiego",
                          m is not None and int(m.group(2)) == 3)

                    q64.wait_serial("proton64>", timeout=40)
                    prog = q64.polecenie("run hello64", "kod wyjscia",
                                         timeout=40)
                    check("program w ring 3 dziala jak dotad",
                          "[hello64] koncze sie grzecznie" in prog)
                finally:
                    q64.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)
            for f in (iso, slog64):
                if os.path.exists(f):
                    os.unlink(f)

    # --- Jeden rdzen: nic sie nie zmienia ---
    print("--- maszyna jednordzeniowa ---")
    slog1 = tempfile.mktemp(suffix=".log")
    q1 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog1)
    try:
        start = q1.wait_for("Procesory:", timeout=60)
        check("na jednym rdzeniu system mowi to wprost i nikogo nie budzi",
              "Procesory: 1" in start)
        q1.wait_for("ProtonOS shell", timeout=30)
        up = q1.polecenie("uptime", "tyknac timera")
        check("i dziala tak samo", "tyknac timera" in up)
    finally:
        q1.close()
        if os.path.exists(slog1):
            os.unlink(slog1)

    return wynik()


if __name__ == "__main__":
    sys.exit(main())
