#!/usr/bin/env python3
"""Test dymny ProtonOS: wymiana stron na dysk w jadrze 64-bitowym.

Program bierze wiecej pamieci, niz maszyna ma RAM-u. Jadro musi po
drodze wyrzucic czesc jego stron na dysk i sciagnac je z powrotem, gdy
program po nie siegnie - a program nie moze tego zauwazyc inaczej niz
chwila zwloki: kazdy bajt ma wrocic taki, jaki byl.

Sprawdzamy jedno i drugie: program potwierdza, ze wszystkie strony
wrocily w calosci, a jadro pokazuje w 'mem', ile ich naprawde poszlo
na dysk. Bez tej drugiej asercji test przeszedlby takze wtedy, gdyby
pamiec zwyczajnie sie zmiescila i wymiana nie ruszyla ani razu.

Magazyn (kernel/swap.c) jest wspolny z jadrem 32-bitowym. W trybie
dlugim doszla polityka wyrzucania: obchod czterech poziomow tablic
stron z bitem 'odwiedzona' i wpis strony na dysku, ktorego procesor
nie oglada, bo nie ma w nim bitu obecnosci.
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

RAM_MIB = 16          # mniej, niz program zaraz wezmie
REQUEST_MIB = 18


def main():
    print("--- wymiana stron na dysk (jadro 64-bitowe) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(64 * 1024 * 1024)
    try:
        check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        # Dysk za kontrolerem AHCI: strony ida na niego przez DMA
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, ram_mib=RAM_MIB, qemu="qemu-system-x86_64")
        try:
            start = q.wait_serial("Wymiana stron:", timeout=90)
            check("plik wymiany zalozony na dysku",
                  re.search(r"Wymiana stron: \d+ KiB na dysku \(swap\.sys\)",
                            start) is not None)

            q.wait_serial("proton64>", timeout=30)
            q.type_line("run swaptest64 %d" % REQUEST_MIB)
            out = q.wait_serial("[swaptest64] wszystkie", timeout=240)
            check("program dostal wiecej pamieci, niz maszyna ma RAM-u",
                  "[swaptest64] zapisuje %d MiB" % REQUEST_MIB in out)
            check("wszystkie strony wrocily z dysku bez zmian",
                  "stron wrocilo w calosci" in out)

            # Bez tego test przeszedlby takze wtedy, gdyby wymiana
            # w ogole nie ruszyla
            q.type_line("mem")
            stan = q.wait_serial("Wymiana:", timeout=20)
            m = re.search(r"Wymiana: \d+/\d+ stron na dysku "
                          r"\(wyrzuconych (\d+), sciagnietych (\d+)\)", stan)
            check("jadro naprawde wyrzucalo strony na dysk",
                  m is not None and int(m.group(1)) > 0)
            check("i sciagalo je z powrotem",
                  m is not None and int(m.group(2)) > 0)

            # Po koncu programu miejsce w pliku wymiany wraca do puli:
            # sprzatanie przestrzeni adresowej zwalnia takze strony,
            # ktore w chwili smierci lezaly na dysku
            used = re.search(r"Wymiana: (\d+)/", stan)
            check("po koncu programu plik wymiany jest pusty",
                  used is not None and int(used.group(1)) == 0)

            # Drugi przebieg na tej samej maszynie: miejsce odzyskane
            # naprawde daje sie uzyc jeszcze raz. Na koncu czekamy na
            # DRUGIE wystapienie napisu, bo pierwsze wisi na porcie od
            # poprzedniego przebiegu.
            q.type_line("run swaptest64 %d" % REQUEST_MIB)
            end = time.time() + 240
            second = ""
            while time.time() < end:
                second = q.serial()
                if second.count("stron wrocilo w calosci") >= 2:
                    break
                time.sleep(1.0)
            check("druga proba na tej samej maszynie tez przechodzi",
                  second.count("stron wrocilo w calosci") == 2)
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, disk):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
