#!/usr/bin/env python3
"""Test dymny ProtonOS: tryb graficzny w jadrze 64-bitowym.

Rysunek sprawdzajacy jest prosty celowo: cztery prostokaty w rogach,
ramka i napis wlasna czcionka. Kazdy blad w liczeniu adresu piksela
widac na nim od razu - zly bajt na piksel rozjezdza obraz w skos,
a zamienione kanaly widac po kolorach.

Sprawdzamy pod BIOS-em i pod UEFI, bo to wlasnie firmware decyduje,
ile bitow ma piksel, a 24 bity to przypadek, w ktorym zapis calym
slowem zadeptuje sasiada.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"

# Kolory palety video.c: 1 czerwony, 2 zielony, 3 niebieski,
# 4 bialy, 5 zolty
RED = (255, 0, 0)
GREEN = (0, 255, 0)
BLUE = (0, 0, 255)
BIALY = (255, 255, 255)
YELLOW = (255, 255, 0)


def near(a, b, tol=30):
    return all(abs(a[i] - b[i]) <= tol for i in range(3))


def inspect(iso, via_uefi):
    """Rysuje wzorzec i sprawdza, gdzie wyladowaly kolory."""
    label = "UEFI" if via_uefi else "BIOS"
    slog = tempfile.mktemp(suffix=".log")
    nvram = tempfile.mktemp(suffix=".fd")
    uefi = None
    if via_uefi:
        shutil.copy(OVMF_VARS, nvram)
        uefi = (OVMF_CODE, nvram)

    q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, uefi=uefi,
             qemu="qemu-system-x86_64")
    try:
        q.wait_serial("proton64>", timeout=60)
        q.type_line("gfx")
        out = q.wait_serial("gfx: rysuje", timeout=30)
        m = re.search(r"gfx: rysuje (\d+)x(\d+), (\d+) bitow", out)
        check(f"{label}: jadro rysuje na buforze od firmware"
              + (f" ({m.group(1)}x{m.group(2)}, {m.group(3)} b/px)"
                 if m else ""),
              m is not None)
        if not m:
            return
        time.sleep(1.5)

        w, h, at = q.screen_raw()
        # Rogi: kazdy inny kolor, wiec pomylka w adresie piksela
        # przestawia je miedzy soba albo rozmazuje
        check(f"{label}: lewy gorny rog czerwony",
              near(at(w // 8, h // 8), RED))
        check(f"{label}: prawy gorny rog zielony",
              near(at(w - w // 8, h // 8), GREEN))
        check(f"{label}: lewy dolny rog zolty",
              near(at(w // 8, h - h // 8), YELLOW))
        check(f"{label}: prawy dolny rog bialy",
              near(at(w - w // 8, h - h // 8), BIALY))
        check(f"{label}: tlo miedzy rogami niebieskie",
              near(at(w // 2, h // 8), BLUE))

        # Napis wlasna czcionka: liczymy jasne piksele w jego pasie.
        # Zly bajt na piksel zostawilby tam smieci albo pustke.
        pas = sum(1 for y in range(h // 2, h // 2 + 8)
                  for x in range(w // 2 - 120, w // 2 + 120)
                  if near(at(x, y), BIALY))
        check(f"{label}: napis narysowany wlasna czcionka "
              f"({pas} pikseli)", 100 < pas < 1500)
    finally:
        q.close()
        for f in (slog, nvram):
            if os.path.exists(f):
                os.unlink(f)


def main():
    print("--- tryb graficzny (jadro 64-bitowe) ---")

    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("POMINIETE: brak obrazu 64-bitowego (make kernel64)")
        return 0
    if shutil.which("grub-mkrescue") is None:
        print("POMINIETE: brak grub-mkrescue")
        return 0
    if shutil.which("qemu-system-x86_64") is None:
        print("POMINIETE: brak qemu-system-x86_64")
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    try:
        os.makedirs(d + "/boot/grub")
        shutil.copy(KERNEL64, d + "/boot/protonos64.bin")
        mod = ""
        if INITRD64 and os.path.exists(INITRD64):
            shutil.copy(INITRD64, d + "/boot/initrd64.img")
            mod = "    module2 /boot/initrd64.img initrd64\n"
        with open(d + "/boot/grub/grub.cfg", "w") as fh:
            fh.write("insmod all_video\nset gfxpayload=keep\n"
                     "set timeout=0\nset default=0\n"
                     'menuentry "ProtonOS 64" {\n'
                     "    multiboot2 /boot/protonos64.bin\n" + mod + "}\n")
        made = subprocess.run(["grub-mkrescue", "-o", iso, d],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
        check("plyta z jadrem 64-bitowym buduje sie", made)
        if not made:
            return summary()

        inspect(iso, False)

        if (os.path.exists(OVMF_CODE) and os.path.exists(OVMF_VARS)
                and os.path.isdir("/usr/lib/grub/x86_64-efi")
                and shutil.which("mformat")):
            inspect(iso, True)
        else:
            print("POMINIETE: brak OVMF albo GRUB-a dla EFI")
    finally:
        shutil.rmtree(d, ignore_errors=True)
        if os.path.exists(iso):
            os.unlink(iso)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
