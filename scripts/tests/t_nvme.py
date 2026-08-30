#!/usr/bin/env python3
"""Test dymny ProtonOS: dysk NVMe (PCI Express).

Warstwa blokowa istniala od czasow AHCI: system plikow i wymiana stron
nie wiedza, ktory kontroler siedzi pod spodem. NVMe jest trzecim
sterownikiem pod ta sama nazwa i najbardziej niepodobnym do reszty -
nie ma w nim nic z dziedzictwa ATA. Zamiast rejestrow i ramek FIS sa
dwie kolejki w pamieci: my wkladamy polecenia, kontroler odklada
odpowiedzi, a o tym, ze cos wrocilo, mowi bit fazy.

Sprawdzamy to tak, jak sprawdza sie dysk: zapisujemy, czytamy, a potem
uruchamiamy maszyne DRUGI RAZ na tym samym obrazie. Plik, ktory
przezyl restart, naprawde lezal na talerzu, a nie w pamieci.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def main():
    # --- Jadro 32-bitowe ---
    print("--- dysk NVMe (jadro 32-bitowe) ---")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    slog = tempfile.mktemp(suffix=".log")

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog, nvme=True)
    try:
        start = q.wait_for("+ PFS gotowy", timeout=60)
        check("warstwa blokowa wybrala NVMe zamiast ATA",
              "Dysk NVMe (PCIe, kolejki) + PFS gotowy" in start)
        check("system plikow sformatowal pusty dysk NVMe",
              "sformatowany" in start)
        # Shell rusza dopiero po schedulerze, a klawisze wpisane
        # wczesniej przepadaja - to nie jest rzecz do zgadywania
        q.wait_for("ProtonOS shell", timeout=30)
        q.wait_for("[siec] DHCP", timeout=30)

        # Wymiana stron zaklada swoj plik juz po systemie plikow,
        # wiec pytamy o nia dopiero teraz
        check("plik wymiany zalozony na dysku NVMe",
              re.search(r"Wymiana stron: \d+ KiB",
                        q.wait_serial("Wymiana stron:", timeout=20))
              is not None)

        write_out = q.command("write nvmetest tresc-przez-kolejki", "zapisano")
        check("zapis pliku na NVMe", "zapisano" in write_out)
        read_res = q.command("cat nvmetest", "tresc-przez-kolejki")
        check("odczyt tego samego pliku oddaje te same bajty",
              "tresc-przez-kolejki" in read_res)

        # Proces w ring 3 przez te sama droge: syscall -> PFS -> NVMe
        ring3 = q.command("run fileio", "[fileio] OK", timeout=40)
        check("proces ring3 pisze i czyta plik przez NVMe",
              "odczytano z dysku: plik zapisany przez proces ring3" in ring3)

        # Duzy plik: kilkanascie partii po jednej stronie kazda -
        # tu widac, czy petla dzielaca transfer liczy sie poprawnie
        big = q.command("run memtest", "[memtest]", timeout=60)
        check("system zyje po intensywnym ruchu na dysku",
              "[memtest]" in big)
    finally:
        q.close()
        if os.path.exists(slog):
            os.unlink(slog)

    # --- Drugi rozruch na tym samym obrazie ---
    print("--- NVMe: drugi rozruch, ten sam obraz ---")
    slog2 = tempfile.mktemp(suffix=".log")
    q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog2, nvme=True)
    try:
        start = q2.wait_for("+ PFS gotowy", timeout=60)
        check("drugi rozruch: dysk juz sformatowany",
              "sformatowany" not in start)
        q2.wait_for("ProtonOS shell", timeout=30)
        q2.wait_for("[siec] DHCP", timeout=30)
        read_res = q2.command("cat nvmetest", "tresc-przez-kolejki")
        check("plik przetrwal restart - naprawde lezal na dysku",
              "tresc-przez-kolejki" in read_res)
        listing = q2.command("fls", "ring3.txt")
        check("plik zapisany przez ring3 tez przetrwal",
              "ring3.txt" in listing)
    finally:
        q2.close()
        for f in (disk, slog2):
            if os.path.exists(f):
                os.unlink(f)

    # --- Jadro 64-bitowe ---
    print("--- dysk NVMe (jadro 64-bitowe) ---")
    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog64 = tempfile.mktemp(suffix=".log")
    disk64 = tempfile.mktemp(suffix=".img")
    with open(disk64, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    try:
        check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        q64 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog64, disk=disk64,
                   nvme=True, qemu="qemu-system-x86_64")
        try:
            # Czekamy na CALY wiersz, a nie na sam skrot "PFS": ten
            # pojawia sie juz przy formatowaniu dysku, wiec czekanie na
            # niego bylo wyscigiem z wypisem o sterowniku
            start = q64.wait_serial("+ PFS gotowy", timeout=90)
            check("jadro 64-bitowe tez widzi dysk NVMe",
                  "Dysk NVMe (PCIe, kolejki) + PFS gotowy" in start)
            q64.wait_serial("proton64>", timeout=30)

            q64.command("write plik64 tresc-z-trybu-dlugiego", "write:")
            read_res = q64.command("cat plik64", "tresc-z-trybu-dlugiego")
            check("zapis i odczyt przez NVMe w trybie dlugim",
                  "tresc-z-trybu-dlugiego" in read_res)

            # Program z dysku NVMe: loader ELF czyta go ta sama droga
            q64.command("cp hello64 znvme", "cp:")
            run_out = q64.command("run znvme", "[hello64] koncze",
                                  timeout=40)
            check("program uruchomiony wprost z dysku NVMe",
                  "Program 'znvme' z dysku" in run_out)
        finally:
            q64.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog64, disk64):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
