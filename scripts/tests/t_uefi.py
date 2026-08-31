#!/usr/bin/env python3
"""ProtonOS smoke test: booting through UEFI (OVMF).

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
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
from ptest import check, summary


def main():
    # things: that the kernel came up at all and that it drew something on
# the screen.
    print()
    print("--- rozruch przez UEFI (OVMF) ---")
    OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
    OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("POMINIETE: brak obrazu 64-bitowego (make kernel64)")
    elif shutil.which("grub-mkrescue") is None:
        print("POMINIETE: brak grub-mkrescue")
    elif not os.path.isdir("/usr/lib/grub/x86_64-efi"):
        print("POMINIETE: brak GRUB-a dla EFI "
              "(apt install grub-efi-amd64-bin)")
    elif shutil.which("mformat") is None:
        print("POMINIETE: brak mtools (partycji EFI nie ma z czego zlozyc)")
    elif not os.path.exists(OVMF_CODE) or not os.path.exists(OVMF_VARS):
        print("POMINIETE: brak firmware OVMF (apt install ovmf)")
    elif shutil.which("qemu-system-x86_64") is None:
        print("POMINIETE: brak qemu-system-x86_64")
    else:
        due = tempfile.mkdtemp()
        isoue = tempfile.mktemp(suffix=".iso")
        slogue = tempfile.mktemp(suffix=".log")
        nvram = tempfile.mktemp(suffix=".fd")
        try:
            os.makedirs(due + "/boot/grub")
            shutil.copy(KERNEL64, due + "/boot/protonos64.bin")
            mod = ""
            if INITRD64 and os.path.exists(INITRD64):
                shutil.copy(INITRD64, due + "/boot/initrd64.img")
                mod = "    module2 /boot/initrd64.img initrd64\n"
            # all_video + gfxpayload=keep: without those GRUB under UEFI
            # cannot set a graphics mode by itself and ends with the error
            # "no suitable video mode found"
            with open(due + "/boot/grub/grub.cfg", "w") as fh:
                fh.write("insmod all_video\nset gfxpayload=keep\n"
                         "set timeout=0\nset default=0\n"
                         'menuentry "ProtonOS 64" {\n'
                         "    multiboot2 /boot/protonos64.bin\n"
                         + mod + "}\n")
            made = subprocess.run(["grub-mkrescue", "-o", isoue, due],
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL).returncode == 0
            check("plyta hybrydowa (BIOS + UEFI) buduje sie", made)
            # The EFI partition really has to be inside, otherwise the
            # firmware has nothing to start - we look for its signature in
            # the image
            if made:
                with open(isoue, "rb") as fh:
                    head = fh.read(4 * 1024 * 1024)
                check("obraz zawiera partycje EFI z programem rozruchowym",
                      b"EFI PART" in head or b"BOOTX64" in head.upper())
            if made:
                # The firmware writes variables, so it works on a copy
                shutil.copy(OVMF_VARS, nvram)
                q24 = Qemu(KERNEL, INITRD, cdrom=isoue, serial=slogue,
                           qemu="qemu-system-x86_64",
                           uefi=(OVMF_CODE, nvram))
                try:
                    out = q24.wait_serial("Multiboot OK", timeout=60)
                    check("firmware UEFI uruchamia GRUB-a, a GRUB jadro",
                          "Jadro uruchomione w trybie dlugim" in out)
                    check("jadro widzi znaczniki Multiboot 2 od GRUB-a EFI",
                          "Multiboot OK (Multiboot 2)" in out)
                    # Under UEFI the ACPI tables are the only way to learn
                    # anything about the board
                    acpi = q24.wait_serial("Wskaznik ACPI", timeout=20)
                    check("loader podaje wskaznik ACPI",
                          re.search(r"Wskaznik ACPI od loadera: [0-9a-f]+",
                                    acpi) is not None)
                    fbl = q24.wait_serial("Bufor obrazu", timeout=20)
                    m = re.search(r"Bufor obrazu: (\d+)x(\d+), (\d+) "
                                  r"bitow na piksel", fbl)
                    check("bufor obrazu od firmware przyjety"
                          + (f" ({m.group(1)}x{m.group(2)}, {m.group(3)} b/px)"
                             if m else ""),
                          m is not None)
                    check("format piksela inny niz 32-bitowy tez dziala",
                          m is not None and m.group(3) in ("24", "32"))
                    mem = q24.wait_serial("Pamiec fizyczna", timeout=20)
                    check("mapa pamieci od GRUB-a EFI ma sens",
                          re.search(r"Pamiec fizyczna: \d{5,} KiB RAM",
                                    mem) is not None)
                    r3 = q24.wait_serial("Program w ring 3:", timeout=40)
                    check("program w ring 3 rusza tak samo jak pod BIOS-em",
                          "pierscien 3" in r3)
                    if INITRD64 and os.path.exists(INITRD64):
                        # The module from GRUB EFI arrives by the same path
                        # as under BIOS - through a Multiboot 2 tag
                        elf = q24.wait_serial("ELF64 z initrd:", timeout=40)
                        check("program z initrd dziala takze pod UEFI",
                              "pierscien 3" in elf
                              and "kod wyjscia 7" in elf)
                    q24.wait_serial("System 64-bitowy gotowy", timeout=30)
                    # And the same on the screen - drawn by our code,
                    # pixel by pixel, with no help from the firmware
                    w, h, at = q24.screen_raw()
                    x0, y0 = (w - 640) // 2, (h - 200) // 2
                    lit = sum(1 for y in range(y0, y0 + 200)
                              for x in range(x0, x0 + 640)
                              if at(x, y) != (0, 0, 0))
                    check(f"konsola rysuje sie na ekranie UEFI "
                          f"({lit} pikseli)", lit > 2000)
                finally:
                    q24.close()
        finally:
            shutil.rmtree(due, ignore_errors=True)
            for f in (isoue, slogue, nvram):
                if os.path.exists(f):
                    os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
