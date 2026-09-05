#!/usr/bin/env python3
"""ProtonOS smoke test: several disks, and choosing between them.

The block layer used to find one disk and that was the disk. It is a
list now, and the reason is the installer: on a machine with more than
one disk, "which one" is the whole question, and a choice that cannot
be made is not a choice.

Two things matter here and both are easy to get wrong. That the list
is the machine's real list - every AHCI port with something on it, and
the old bus behind them, in an order that leaves a machine with ONE
disk behaving exactly as it did. And that LOOKING at another disk
changes nothing: reading its table means pointing the machine at it,
and a machine left pointed somewhere it was never mounted from is a
filesystem reading a stranger's sectors.

Run on its own or through scripts/run_tests.py.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, no64, make_gpt_disk, mtools_ok, fat_format,
                   fat_mkdirs, fat_put, fat_get, fat_kind, gpt_read,
                   GUID_ESP4, GUID_PFS4)

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
LOADER = os.path.join(ROOT, "build", "BOOTX64.EFI")


def blob(path):
    with open(path, "rb") as fh:
        return fh.read()


def main():
    print()
    print("--- kilka dyskow i wybor miedzy nimi ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()
    if not mtools_ok():
        print("POMINIETE: brak mtools")
        return summary()
    if not os.path.exists(LOADER):
        subprocess.run(["make", "efi"], cwd=ROOT, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    if not os.path.exists(LOADER):
        print("POMINIETE: brak build/BOOTX64.EFI (apt install clang lld)")
        return summary()
    if not os.path.exists(OVMF_CODE) or not os.path.exists(OVMF_VARS):
        print("POMINIETE: brak firmware OVMF")
        return summary()
    if shutil.which("qemu-system-x86_64") is None:
        print("POMINIETE: brak qemu-system-x86_64")
        return summary()

    esp = tempfile.mkdtemp()
    first = tempfile.mktemp(suffix=".img")
    second = tempfile.mktemp(suffix=".img")
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")
    nvram = tempfile.mktemp(suffix=".fd")

    want_kernel = blob(KERNEL64)

    try:
        os.makedirs(os.path.join(esp, "EFI", "BOOT"))
        shutil.copy(LOADER, os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI"))
        shutil.copy(KERNEL64, os.path.join(esp, "protonos64.bin"))
        shutil.copy(INITRD64, os.path.join(esp, "initrd64.img"))

        # The first disk belongs to somebody: a table, an EFI partition,
        # their loader on it. The second is 400 MiB of nothing.
        make_gpt_disk(first)
        fat_format(first, 2048, 204800, fat32=True, label="LAPTOPESP")
        fat_mkdirs(first, 2048, ["/EFI", "/EFI/Microsoft"])
        fat_put(first, 2048, b"MZ-cudzy" * 400,
                "/EFI/Microsoft/bootmgfw.efi")
        with open(second, "wb") as fh:
            fh.truncate(400 * 1024 * 1024)

        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, esp=esp, disk=first, disk2=second,
                 sata=True, serial=slog, qemu="qemu-system-x86_64",
                 uefi=(OVMF_CODE, nvram))
        try:
            q.wait_serial("System 64-bitowy gotowy", timeout=120)

            out = q.command("instaluj tekstem", "Wpisz TAK", timeout=40)
            check("system wymienia dyski, ktore ma",
                  "dysk 1: AHCI" in out and "dysk 2: AHCI" in out)
            check("i pokazuje, na ktory patrzy", "800 MiB  <-" in out)
            check("pierwszy jest tym, ktorego uzywa - jak dotad",
                  "Dysk 1 z" in out and "LAPTOPESP" in out)
            q.command("nie", "instaluj:", timeout=20)

            # The second disk: no table, so there is nothing to install
            # beside - and the system says which disk it is talking
            # about rather than which disk it happens to be using
            out = q.command("instaluj tekstem 2", "instaluj:", timeout=40)
            check("mozna zapytac o drugi dysk",
                  "400 MiB  <-" in out)
            check("i o drugim mowi prawde: nie ma na nim tablicy",
                  "nie ma tablicy partycji" in out)

            # And nothing moved: asked again without a number, the
            # system talks about the disk it is USING, which is still
            # the first one. Looking at another disk means pointing
            # the machine at it, and a machine left pointed somewhere
            # it was never mounted from is a filesystem reading a
            # stranger's sectors.
            out = q.command("instaluj tekstem", "Wpisz TAK", timeout=40)
            check("po ogladaniu drugiego dysku system dalej uzywa "
                  "pierwszego",
                  "Dysk 1 z" in out and "800 MiB  <-" in out)
            q.command("nie", "instaluj:", timeout=20)

            # The 'disk' command writes a pattern into a sector and
            # reads it back, and the sector it used was the last one -
            # which was safe while our disks had nothing on them. On a
            # GPT disk the last sector IS the copy of the partition
            # table, and a write there destroys it silently: the disk
            # still boots, and one day a repair tool says the table is
            # damaged. This test found that; the assertion is what
            # keeps it found.
            out = q.command("disk", "proton64>", timeout=20)
            check("'disk' na dysku z tablica tylko ja czyta",
                  "tylko go czytam" in out)
            ok, after, why2 = gpt_read(first)
            check("i kopia tablicy na koncu dysku zyje%s"
                  % ("" if ok else ": " + why2), ok and len(after) == 3)

            # Now install onto the second, wiping it - and the first
            # one must come out of it untouched
            q.command("instaluj tekstem caly 2", "Wpisz KASUJ", timeout=40)
            out = q.command("KASUJ", "Gotowe", timeout=240)
            check("instalacja na wskazany dysk dochodzi do konca",
                  "tablica partycji (GPT) - gotowe" in out
                  and "/EFI/BOOT/BOOTX64.EFI - gotowe" in out)
        finally:
            q.close()

        # --- what is on each of them, read here
        ok, parts, why = gpt_read(second)
        check("drugi dysk ma teraz nasza tablice%s"
              % ("" if ok else ": " + why), ok and len(parts) == 2)
        if len(parts) == 2:
            check("z partycja EFI i nasza", parts[0][0] == GUID_ESP4
                  and parts[1][0] == GUID_PFS4)
        check("na jego partycji EFI jest FAT32",
              fat_kind(second, 2048) == "FAT32")
        got = fat_get(second, 2048, "/EFI/ProtonOS/protonos64.bin")
        check("i jadro co do bajtu (%d B)" % len(want_kernel),
              got is not None and got == want_kernel)

        ok, still, why2 = gpt_read(first)
        check("PIERWSZY dysk jest nietkniety - ta sama tablica (%s)"
              % (why2 or ", ".join(e[3] for e in still)),
              ok and len(still) == 3 and still[1][3] == "Windows")
        check("i cudzy loader dalej na nim jest",
              (fat_get(first, 2048, "/EFI/Microsoft/bootmgfw.efi") or b"")
              .startswith(b"MZ-cudzy"))
        check("a nasze pliki NIE trafily na niego",
              fat_get(first, 2048, "/EFI/ProtonOS/protonos64.bin") is None)

        # --- and it boots, from the disk that was chosen
        shutil.copy(OVMF_VARS, nvram)
        q2 = Qemu(KERNEL, INITRD, disk=second, sata=True, serial=slog2,
                  qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            out = q2.wait_serial("protoboot: ProtonOS", timeout=60)
            check("wskazany dysk startuje sam", "protoboot" in out)
            out = q2.wait_serial("System 64-bitowy gotowy", timeout=120)
            check("i system z niego dochodzi do powloki",
                  "Multiboot OK (Multiboot 2)" in out)
        finally:
            q2.close()
    finally:
        shutil.rmtree(esp, ignore_errors=True)
        for f in (first, second, slog, slog2, nvram):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
