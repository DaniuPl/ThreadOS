#!/usr/bin/env python3
"""ProtonOS smoke test: installing onto the machine's own disk (in text).

The installer proper takes the whole screen and is checked in t_ginstal;
this group is the same three files written by the same code, said out
loud in the console - which is what one wants when there is no screen
to draw on, or when the answer has to end up in a log.

The shape of this group is the shape of the thing: a machine started
from a stick, a second disk that belongs to somebody else, and the
question of whether what lands on it is right and whether what was
already there is still there.

Nothing here trusts the system's own account of what it did. Every file
it writes is read back off the image by mtools and compared byte for
byte, and at the end the target disk is booted on its own - which is
the only proof that installing installed anything.

Run on its own or through scripts/run_tests.py.
"""

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, no64, make_gpt_disk, gpt_intact,
                   neighbours_intact, mtools_ok, fat_format, fat_mkdirs,
                   fat_put, fat_get, fat_dir)

ESP_START = 2048
ESP_SECTORS = 204800

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
LOADER = os.path.join(ROOT, "build", "BOOTX64.EFI")

FOREIGN_BOOTMGR = b"MZ-to-jest-cudzy-bootmgfw-" * 300
FOREIGN_FALLBACK = b"MZ-cudzy-loader-w-EFI-BOOT-" * 200


def blob(path):
    with open(path, "rb") as fh:
        return fh.read()


def make_target(path, with_fallback):
    """The other machine's disk: a table, an EFI partition with a
    filesystem somebody else made, and somebody else's boot loader in
    it."""
    make_gpt_disk(path)
    if not fat_format(path, ESP_START, ESP_SECTORS, fat32=True,
                      label="LAPTOPESP"):
        return False
    fat_mkdirs(path, ESP_START, ["/EFI", "/EFI/Microsoft",
                                 "/EFI/Microsoft/Boot"])
    fat_put(path, ESP_START, FOREIGN_BOOTMGR,
            "/EFI/Microsoft/Boot/bootmgfw.efi")
    if with_fallback:
        fat_mkdirs(path, ESP_START, ["/EFI/BOOT"])
        fat_put(path, ESP_START, FOREIGN_FALLBACK, "/EFI/BOOT/BOOTX64.EFI")
    return True


def boot_stick(where):
    """The stick the machine is started from: our loader and the two
    files it reads."""
    os.makedirs(os.path.join(where, "EFI", "BOOT"), exist_ok=True)
    shutil.copy(LOADER, os.path.join(where, "EFI", "BOOT", "BOOTX64.EFI"))
    shutil.copy(KERNEL64, os.path.join(where, "protonos64.bin"))
    shutil.copy(INITRD64, os.path.join(where, "initrd64.img"))


def main():
    print()
    print("--- instalacja na dysk maszyny ---")

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
    taken = tempfile.mktemp(suffix=".img")
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")
    slog3 = tempfile.mktemp(suffix=".log")
    nvram = tempfile.mktemp(suffix=".fd")

    want_loader = blob(LOADER)
    want_kernel = blob(KERNEL64)
    want_initrd = blob(INITRD64)

    try:
        boot_stick(esp)
        check("dysk obcej maszyny gotowy (GPT + FAT32 od mtools)",
              make_target(target, with_fallback=False))

        # --- A machine started from the stick, with the other one's
        # disk behind an AHCI controller - which is the disk the system
        # picks, and which is exactly the situation being tested
        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, esp=esp, disk=target, sata=True,
                 serial=slog, qemu="qemu-system-x86_64",
                 uefi=(OVMF_CODE, nvram))
        try:
            out = q.wait_serial("protoboot: ProtonOS", timeout=60)
            check("maszyna wstaje z pendrive'a przez nasz loader",
                  "protoboot" in out)
            head = q.wait_serial("loader:", timeout=30)
            check("loader podaje jadro i SIEBIE jako moduly - bez tego "
                  "nie ma z czego instalowac",
                  f"loader: {len(want_loader)} B" in head)
            q.wait_serial("System 64-bitowy gotowy", timeout=90)

            # Somebody who does not type the word gets nothing done
            out = q.command("instaluj tekstem", "Wpisz TAK", timeout=30)
            check("instalator najpierw mowi, co by zapisal",
                  "/EFI/ProtonOS/protonos64.bin" in out
                  and "Partycja EFI: numer 1" in out)
            check("i obiecuje, czego nie ruszy",
                  "NIE ruszam: tablicy partycji" in out)
            out = q.command("nie", "instaluj:", timeout=20)
            check("bez slowa TAK nie zapisuje niczego",
                  "nie zapisano niczego" in out)

            out = q.command("instaluj tekstem", "Wpisz TAK", timeout=30)
            out = q.command("TAK", "Gotowe", timeout=90)
            check("po TAK zapisuje wszystkie trzy pliki",
                  "/EFI/ProtonOS/BOOTX64.EFI - gotowe" in out
                  and "/EFI/ProtonOS/protonos64.bin - gotowe" in out
                  and "/EFI/ProtonOS/initrd64.img - gotowe" in out)
            check("i - skoro nikogo tam nie bylo - takze /EFI/BOOT/"
                  "BOOTX64.EFI",
                  "/EFI/BOOT/BOOTX64.EFI - gotowe" in out
                  and "uruchomi ProtonOS-a z dysku" in out)
        finally:
            q.close()

        # --- What is really on that disk, according to somebody else
        for path, want in (("/EFI/ProtonOS/BOOTX64.EFI", want_loader),
                           ("/EFI/ProtonOS/protonos64.bin", want_kernel),
                           ("/EFI/ProtonOS/initrd64.img", want_initrd),
                           ("/EFI/BOOT/BOOTX64.EFI", want_loader)):
            got = fat_get(target, ESP_START, path)
            check(f"{path} co do bajtu ({len(want)} B)",
                  got is not None and got == want)

        got = fat_get(target, ESP_START, "/EFI/Microsoft/Boot/bootmgfw.efi")
        check("cudzy bootmgfw.efi nietkniety",
              got == FOREIGN_BOOTMGR)
        check("tablica GPT nietknieta", gpt_intact(target))
        check("sasiednia partycja Windows nietknieta",
              neighbours_intact(target, which=(1,)))

        # --- The proof: the disk boots on its own, with no stick
        shutil.copy(OVMF_VARS, nvram)
        q2 = Qemu(KERNEL, INITRD, disk=target, sata=True, serial=slog2,
                  qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            out = q2.wait_serial("protoboot: ProtonOS", timeout=60)
            check("zainstalowany dysk startuje sam, bez pendrive'a",
                  "protoboot" in out)
            out = q2.wait_serial("oddaje maszyne", timeout=30)
            check(f"loader znajduje jadro tam, gdzie je polozyl instalator "
                  f"({len(want_kernel)} B)",
                  f"jadro:  {len(want_kernel)} B" in out)
            out = q2.wait_serial("System 64-bitowy gotowy", timeout=90)
            check("i system z tego dysku dochodzi do powloki",
                  "Multiboot OK (Multiboot 2)" in out)
        finally:
            q2.close()

        # --- And the other case: a machine that already has a loader
        # under the name the firmware falls back to
        check("drugi dysk - z cudzym /EFI/BOOT/BOOTX64.EFI - gotowy",
              make_target(taken, with_fallback=True))
        shutil.copy(OVMF_VARS, nvram)
        q3 = Qemu(KERNEL, INITRD, esp=esp, disk=taken, sata=True,
                  serial=slog3, qemu="qemu-system-x86_64",
                  uefi=(OVMF_CODE, nvram))
        try:
            q3.wait_serial("System 64-bitowy gotowy", timeout=120)
            out = q3.command("instaluj tekstem", "Wpisz TAK", timeout=30)
            check("instalator widzi, ze /EFI/BOOT/BOOTX64.EFI jest zajety",
                  "JEST JUZ ZAJETY" in out)
            check("i z gory mowi, ze trzeba bedzie wpisu w firmwarze",
                  "menu rozruchowym" in out)
            out = q3.command("TAK", "Gotowe", timeout=90)
            check("zapisuje swoje pliki mimo to",
                  "/EFI/ProtonOS/protonos64.bin - gotowe" in out)
            check("ale nie pisze o zapisaniu cudzego pliku",
                  "/EFI/BOOT/BOOTX64.EFI - gotowe" not in out
                  and "dodaj w firmwarze wpis" in out)
        finally:
            q3.close()

        got = fat_get(taken, ESP_START, "/EFI/BOOT/BOOTX64.EFI")
        check("cudzy loader pod /EFI/BOOT zostal, co do bajtu",
              got == FOREIGN_FALLBACK)
        got = fat_get(taken, ESP_START, "/EFI/ProtonOS/protonos64.bin")
        check("a nasze jadro i tak jest na miejscu",
              got is not None and got == want_kernel)
        listing = fat_dir(taken, ESP_START)
        check("mtools widzi oba katalogi obok siebie",
              "ProtonOS" in listing and "Microsoft" in listing)
    finally:
        shutil.rmtree(esp, ignore_errors=True)
        for f in (target, taken, slog, slog2, slog3, nvram):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
