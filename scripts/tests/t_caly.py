#!/usr/bin/env python3
"""ProtonOS smoke test: taking the whole disk.

The other way of installing. Not three files into somebody's EFI
partition, but a disk made ours from nothing: a new partition table, a
new EFI partition with a FAT32 on it, a partition for ProtonOS - and
everything that was on that disk gone.

So this group is mostly about what a table has to be for a firmware to
believe it. The table is read back HERE, in Python, the way a firmware
reads one: the protective MBR, both headers, both CRCs, and the copy at
the end of the disk that has to match the one at the front. A table
that is merely consistent with the code that wrote it is a table that
boots nothing.

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
                   fat_mkdirs, fat_put, fat_get, fat_dir, fat_kind, gpt_read,
                   GUID_ESP4, GUID_PFS4)

ESP_START = 2048
ESP_SECTORS = 204800

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
    print("--- caly dysk dla ProtonOS-a ---")

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
    target = tempfile.mktemp(suffix=".img")
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")
    nvram = tempfile.mktemp(suffix=".fd")

    want_loader = blob(LOADER)
    want_kernel = blob(KERNEL64)
    want_initrd = blob(INITRD64)

    try:
        os.makedirs(os.path.join(esp, "EFI", "BOOT"))
        shutil.copy(LOADER, os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI"))
        shutil.copy(KERNEL64, os.path.join(esp, "protonos64.bin"))
        shutil.copy(INITRD64, os.path.join(esp, "initrd64.img"))

        # A disk that belongs to somebody else, with a table of its own
        # and data in its partitions. All of it is going away, and the
        # test says so before it happens.
        make_gpt_disk(target)
        fat_format(target, ESP_START, ESP_SECTORS, fat32=True, label="STARY")
        fat_mkdirs(target, ESP_START, ["/EFI", "/EFI/Microsoft"])
        fat_put(target, ESP_START, b"MZ-cudzy" * 400,
                "/EFI/Microsoft/bootmgfw.efi")
        ok, before, why = gpt_read(target)
        check("dysk obcej maszyny ma wlasna, poprawna tablice GPT (%d "
              "partycji)" % len(before), ok and len(before) == 3)

        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, esp=esp, disk=target, sata=True,
                 serial=slog, qemu="qemu-system-x86_64",
                 uefi=(OVMF_CODE, nvram))
        try:
            q.wait_serial("System 64-bitowy gotowy", timeout=120)

            out = q.command("instaluj tekstem caly", "Wpisz KASUJ",
                            timeout=40)
            check("plan mowi wprost, ze skasuje caly dysk",
                  "CALY TEN DYSK BEDZIE ProtonOS-a" in out
                  and "Zniknie z niego WSZYSTKO" in out)
            check("i wymienia, co zrobi",
                  "tablice GPT" in out
                  and "partycje EFI 128 MiB (FAT32)" in out
                  and "ProtonOS-a (PFS)" in out)
            check("slowo jest inne niz przy instalacji obok",
                  "Wpisz KASUJ" in out and "Wpisz TAK" not in out)

            # A word that is not the word - and the disk still has its
            # old table afterwards
            out = q.command("nie", "instaluj:", timeout=20)
            check("bez slowa KASUJ nie dzieje sie nic",
                  "nie zapisano niczego" in out)
            ok, still, _ = gpt_read(target)
            check("i stara tablica jest nietknieta",
                  ok and len(still) == 3 and still[0][3] == before[0][3])

            q.command("instaluj tekstem caly", "Wpisz KASUJ", timeout=40)
            out = q.command("KASUJ", "Gotowe", timeout=180)
            check("po KASUJ robi tablice, partycje i system plikow",
                  "tablica partycji (GPT) - gotowe" in out
                  and "partycja EFI (FAT32) - gotowe" in out
                  and "partycja ProtonOS (PFS) - gotowe" in out)
            check("i zapisuje na nich pliki",
                  "/EFI/ProtonOS/protonos64.bin - gotowe" in out
                  and "/EFI/BOOT/BOOTX64.EFI - gotowe" in out)
        finally:
            q.close()

        # --- the table, read the way a firmware reads one
        ok, parts, why = gpt_read(target)
        check("tablica GPT trzyma sie kupy (MBR ochronny, oba naglowki, "
              "obie sumy)%s" % ("" if ok else ": " + why), ok)
        check("sa dokladnie dwie partycje (%d)" % len(parts),
              len(parts) == 2)
        if len(parts) == 2:
            check("pierwsza to partycja EFI, i mowi to swoim GUID-em",
                  parts[0][0] == GUID_ESP4)
            check("druga jest nasza i tez mowi to swoim GUID-em (%s)"
                  % parts[1][0].decode("latin1"),
                  parts[1][0] == GUID_PFS4)
            check("nazwy partycji: '%s' i '%s'" % (parts[0][3], parts[1][3]),
                  parts[0][3] == "EFI" and parts[1][3] == "ProtonOS")
            check("partycja EFI zaczyna sie na 2048 i ma 128 MiB (%d "
                  "sektorow)" % (parts[0][2] - parts[0][1] + 1),
                  parts[0][1] == 2048
                  and parts[0][2] - parts[0][1] + 1 == 128 * 2048)
            check("nasza partycja zaczyna sie tam, gdzie tamta konczy",
                  parts[1][1] == parts[0][2] + 1)
            size = os.path.getsize(target) // 512
            check("i nie wchodzi na kopie tablicy na koncu dysku",
                  parts[1][2] <= size - 34)

        # --- the filesystem on it, read by somebody else
        check("na partycji EFI jest FAT32 (%s)" % fat_kind(target, 2048),
              fat_kind(target, 2048) == "FAT32")
        listing = fat_dir(target, 2048)
        check("mtools czyta z niej katalogi, ktore zrobilismy",
              "ProtonOS" in listing and "BOOT" in listing)
        for path, want in (("/EFI/ProtonOS/BOOTX64.EFI", want_loader),
                           ("/EFI/ProtonOS/protonos64.bin", want_kernel),
                           ("/EFI/ProtonOS/initrd64.img", want_initrd),
                           ("/EFI/BOOT/BOOTX64.EFI", want_loader)):
            got = fat_get(target, 2048, path)
            check("%s co do bajtu (%d B)" % (path, len(want)),
                  got is not None and got == want)

        # --- and the thing this mode is FOR: the old disk is gone.
        #
        # Gone means the table and the filesystems are gone, not that
        # every byte has been scrubbed - no installer zeroes a disk,
        # and pretending otherwise here would be asserting something
        # that is not true of any system. What must be true is that
        # nothing of the old shape is left: the partitions the old
        # table described are not described by anything any more.
        old_starts = [e[1] for e in before]
        new_starts = [e[1] for e in parts]
        check("po zadnej ze starych partycji nie zostal wpis w tablicy",
              not any(x in new_starts for x in old_starts[1:]))
        check("i cudzego bootmgfw.efi nie da sie juz odczytac",
              fat_get(target, 2048, "/EFI/Microsoft/bootmgfw.efi") is None)

        # --- the proof: it boots
        shutil.copy(OVMF_VARS, nvram)
        q2 = Qemu(KERNEL, INITRD, disk=target, sata=True, serial=slog2,
                  qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            out = q2.wait_serial("protoboot: ProtonOS", timeout=60)
            check("firmware wybiera nasz loader z tablicy, ktora sami "
                  "zrobilismy", "protoboot" in out)
            out = q2.wait_serial("System 64-bitowy gotowy", timeout=120)
            check("i system z tego dysku dochodzi do powloki",
                  "Multiboot OK (Multiboot 2)" in out)
            check("a PFS na naszej partycji jest gotowy do uzycia",
                  "PFS" in out and "NIEDOSTEPNY" not in out.split(
                      "Ustawienia:")[0].split("Dysk ")[-1])
            out = q2.command("fls", "proton64>", timeout=20)
            check("i mozna na nim trzymac pliki",
                  "nie ma" not in out.lower() or "swap" in out.lower())
        finally:
            q2.close()
    finally:
        shutil.rmtree(esp, ignore_errors=True)
        for f in (target, slog, slog2, nvram):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
