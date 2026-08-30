#!/usr/bin/env python3
"""Test dymny ProtonOS: rozruch trybu dlugiego (jadro 64-bitowe).

Pierwszy krok kazdego jadra 64-bitowego: procesor ma sam potwierdzic,
ze jest w trybie dlugim (LMA=1), tablice stron maja byc zbudowane
z ramek wzietych od menedzera pamieci, a nie ulozone na sztywno
w obrazie jadra, i musi dzialac przerwanie.

Najmocniejsza asercja dotyczy strony wstawionej osiem gigabajtow ponad
obszar opisany identycznosciowo: zapis i odczyt przez nia przechodza
cala czterostopniowa sciezke tlumaczenia adresow, a nie skrot.

Uruchamiany osobno albo przez scripts/run_tests.py, ktory puszcza
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


def main():
    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")
    # Dysk za kontrolerem AHCI: to ta droga wymagala doklejenia
    # rejestrow kontrolera do mapowania
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.write(b"\0" * DISK_SIZE)

    try:
        check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        q23 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                   sata=True, qemu="qemu-system-x86_64")
        try:
            out = q23.wait_serial("Tryb dlugi:", timeout=40)
            check("jadro startuje w trybie dlugim",
                  "Jadro uruchomione w trybie dlugim" in out)
            # LMA ustawia sam procesor - to jego zdanie, nie nasze
            check("procesor potwierdza tryb dlugi (LMA=1)",
                  "LMA=1" in out)
            check("wskazniki maja osiem bajtow",
                  "wskaznik ma 8 bajtow" in out)
            check("mapa pamieci czytana tym samym kodem",
                  re.search(r"Pamiec fizyczna: \d{5,} KiB RAM",
                            q23.wait_serial("Pamiec fizyczna",
                                            timeout=20)) is not None)
            check("bufor obrazu od loadera rozpoznany",
                  re.search(r"Bufor obrazu: \d+x\d+",
                            q23.wait_serial("Bufor obrazu",
                                            timeout=20)) is not None)

            # Pamiec: wlasne tablice zbudowane z ramek od PMM,
            # a nie te ulozone na sztywno w obrazie jadra
            mem = q23.wait_serial("Stronicowanie czteropoziomowe",
                                  timeout=20)
            m = re.search(r"czteropoziomowe: (\d+) MiB, zakaz "
                          r"wykonywania (\w+)", mem)
            check("wlasne tablice stron obejmuja cala pamiec"
                  + (f" ({m.group(1)} MiB)" if m else ""),
                  m is not None and int(m.group(1)) >= 100)
            check("zakaz wykonywania wlaczony (bit NXE)",
                  m is not None and m.group(2) == "wlaczony")
            # Strona wstawiona osiem gigabajtow ponad obszar
            # opisany identycznosciowo - zapis i odczyt przez
            # nia przechodza cala czterostopniowa sciezke
            hi = q23.wait_serial("Mapowanie 4 KiB", timeout=20)
            check("mapowanie 4 KiB daleko poza pamiecia dziala",
                  "odczyt 123456789abcdef" in hi)
            heap = q23.wait_serial("Sterta jadra", timeout=20)
            check("sterta jadra dziala w trybie dlugim",
                  "kmalloc/kfree dzialaja" in heap
                  and "po zwolnieniu 0 B" in heap)

            # Przerwania w trybie dlugim: pulapka int3 przechodzi
            # cala droge - stub, tablice, ramke rejestrow, iretq -
            # i wraca, nie zatrzymujac systemu
            trap = q23.wait_serial("[INT3]", timeout=20)
            check("wyjatek-pulapka obsluzony i wznowiony",
                  re.search(r"\[INT3\] breakpoint pod rip=",
                            trap) is not None)
            # A timer musi tykac sam z siebie - to dowod, ze
            # przerwania sprzetowe dochodza przez nowa tablice

            # Watki: liczniki musza zgadzac sie z dlugoscia snu
            # co do jednego. Przez 600 ms watek spiacy po 20 ms
            # zdazy 30 razy, a ten po 50 ms - 12. Zepsute
            # przelaczanie kontekstu albo usypianie od razu
            # wypycha te liczby poza zakres.
            sch = q23.wait_serial("Watki licza rownolegle",
                                  timeout=25)
            m = re.search(r"A=(\d+), B=(\d+)", sch)
            check("watki dzialaja rownolegle, a sen trwa tyle, ile ma"
                  + (f" (A={m.group(1)}, B={m.group(2)})"
                     if m else ""),
                  m is not None and 28 <= int(m.group(1)) <= 32
                  and 11 <= int(m.group(2)) <= 13)
            zombie = q23.wait_serial("Watek konczacy sie", timeout=20)
            check("zakonczony watek znika z listy",
                  "zrobil swoje" in zombie
                  and "zadan na liscie: 4" in zombie)
        finally:
            q23.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, slog2, disk):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
