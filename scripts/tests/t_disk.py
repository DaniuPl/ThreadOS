#!/usr/bin/env python3
"""Test dymny ProtonOS: uruchamianie programow z dysku.

Loader ELF i system plikow istnialy obok siebie od dawna i nigdy nie
byly polaczone: 'run' zagladal wylacznie do initrd, czyli do obrazu
wgrywanego razem z jadrem i tylko do odczytu. Teraz program moze lezec
na dysku - i nadal daje sie uruchomic.

Najmocniejszy dowod jest taki sam, jak przy systemie plikow: DRUGI
ROZRUCH tej samej maszyny z tym samym obrazem dysku. Program, ktory
uruchamia sie po restarcie, a ktorego nie ma w initrd, moze pochodzic
juz tylko z talerza.

Szukanie programu (kernel/prog.c) jest wspolne dla obu jader - o trybie
procesora nie ma w nim ani slowa - wiec sprawdzamy je po obu stronach.
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

# Nazwa, ktorej w initrd NIE MA - inaczej nie wiadomo, skad program
# przyszedl
ON_DISK = "zdysku"


def main():
    print("--- programy z dysku (jadro 64-bitowe) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        # --- Pierwszy rozruch: program trafia z initrd na dysk ---
        slog = tempfile.mktemp(suffix=".log")
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, qemu="qemu-system-x86_64")
        try:
            q.wait_serial("PFS gotowy", timeout=90)
            q.wait_serial("proton64>", timeout=30)

            missing = q.command("run " + ON_DISK, "run:")
            check("przed skopiowaniem programu na dysku nie ma",
                  "run: nie ma programu" in missing)

            copied = q.command("cp hello64 " + ON_DISK, "cp:")
            check("'cp' przepisuje program z initrd na dysk",
                  "cp: 'hello64' -> '%s'" % ON_DISK in copied)

            listing = q.command("fls", "  %s  (" % ON_DISK)
            check("skopiowany program widac w spisie plikow dysku",
                  "  %s  (" % ON_DISK in listing)

            wyn = q.command("run " + ON_DISK, "[hello64] koncze",
                              timeout=30)
            check("program z dysku uruchamia sie i dziala",
                  "[hello64] czesc" in wyn)
            check("jadro melduje, ze wzielo go z dysku, a nie z initrd",
                  "Program '%s' z dysku" % ON_DISK in wyn)
        finally:
            q.close()
            if os.path.exists(slog):
                os.unlink(slog)

        # --- Drugi rozruch z tym samym dyskiem ---
        slog2 = tempfile.mktemp(suffix=".log")
        q2 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog2, disk=disk,
                  sata=True, qemu="qemu-system-x86_64")
        try:
            q2.wait_serial("proton64>", timeout=90)
            po = q2.command("run " + ON_DISK, "[hello64] koncze",
                              timeout=30)
            check("program przetrwal restart i dalej sie uruchamia",
                  "Program '%s' z dysku" % ON_DISK in po)

            # Program z dysku jest zwyklym programem: ma wlasna
            # przestrzen adresowa i konczy sie wlasnym kodem wyjscia
            check("program z dysku konczy sie wlasnym kodem wyjscia",
                  "koniec, kod wyjscia 7" in po)

            # ...i drugi raz tez, czyli bufor po nim zostal oddany.
            # Liczymy tylko to, co przybylo po TYM poleceniu: hello64
            # startuje takze przy rozruchu, wiec na porcie jest go
            # wiecej niz raz.
            two = q2.command("run " + ON_DISK, "[hello64] koncze",
                               timeout=30)
            check("drugie uruchomienie z dysku tez przechodzi",
                  two.count("[hello64] koncze") == 1)

            # Ta sama droga z pierscienia 3: shell uzytkownika robi
            # fork i exec, a exec szuka programu tak samo jak jadro -
            # wiec program z dysku uruchamia sie takze stamtad
            q2.command("run ush64", "ush64$")
            from_ring3 = q2.command(ON_DISK, "[hello64] koncze", timeout=30)
            check("exec z pierscienia 3 tez znajduje program na dysku",
                  "[hello64] czesc" in from_ring3)
            q2.command("exit", "koniec, kod wyjscia")

            # Nazwa, ktorej nie ma nigdzie, dalej jest bledem
            bad_one = q2.command("run niczegotakiego", "run: nie ma programu")
            check("nieistniejacy program dalej jest bledem",
                  "run: nie ma programu" in bad_one)
        finally:
            q2.close()
            if os.path.exists(slog2):
                os.unlink(slog2)
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, disk):
            if os.path.exists(f):
                os.unlink(f)

    # --- To samo w jadrze 32-bitowym ---
    # Ten sam plik szukajacy programu, ta sama droga; rozni sie tylko
    # loader ELF po drugiej stronie
    print("--- programy z dysku (jadro 32-bitowe) ---")
    disk32 = tempfile.mktemp(suffix=".img")
    slog32 = tempfile.mktemp(suffix=".log")
    with open(disk32, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        q3 = Qemu(KERNEL, INITRD, disk32, append=TEXT_MODE, serial=slog32)
        try:
            q3.wait_for("ProtonOS shell", timeout=40)
            copied = q3.command("cp hello " + ON_DISK, "cp:")
            check("'cp' przepisuje program na dysk (32-bit)",
                  "cp: 'hello' -> '%s'" % ON_DISK in copied)
            wyn = q3.command("run " + ON_DISK, "[hello] koncze",
                               timeout=30)
            check("program z dysku uruchamia sie (32-bit)",
                  "[hello] czesc" in wyn)
            check("jadro 32-bitowe tez melduje, skad go wzielo",
                  "exec '%s' z dysku" % ON_DISK in wyn)
        finally:
            q3.close()
    finally:
        for f in (disk32, slog32):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
