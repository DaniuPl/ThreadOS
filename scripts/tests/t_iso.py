#!/usr/bin/env python3
"""Test dymny ProtonOS: rozruch z plyty (Multiboot 2).

Uruchamiany osobno albo przez scripts/run_testy.py, ktory puszcza
wszystkie grupy rownolegle.
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
    # ACPI przychodza jako znaczniki Multiboot 2.
    print("--- rozruch z ISO (Multiboot 2) ---")
    if shutil.which("grub-mkrescue") is None:
        print("POMINIETE: brak grub-mkrescue "
              "(apt install grub-pc-bin grub-common xorriso)")
    else:
        isodir = tempfile.mkdtemp()
        iso = tempfile.mktemp(suffix=".iso")
        try:
            os.makedirs(isodir + "/boot/grub")
            shutil.copy(KERNEL, isodir + "/boot/protonos.bin")
            shutil.copy(INITRD, isodir + "/boot/initrd.img")
            # Wlasne menu: bez odliczania i od razu w tryb tekstowy,
            # zeby dalo sie czytac ekran
            with open(isodir + "/boot/grub/grub.cfg", "w") as fh:
                fh.write("set timeout=0\nset default=0\n"
                         'menuentry "ProtonOS" {\n'
                         "    multiboot2 /boot/protonos.bin nodesktop\n"
                         "    module2 /boot/initrd.img\n}\n")
            made = subprocess.run(["grub-mkrescue", "-o", iso, isodir],
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL).returncode == 0
            check("obraz ISO buduje sie", made)
            if made:
                slog = tempfile.mktemp(suffix=".log")
                q21 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog)
                try:
                    boot = q21.wait_serial("Multiboot OK", timeout=40)
                    check("jadro wgrane przez GRUB-a mowi Multiboot 2",
                          "Multiboot OK (Multiboot 2)" in boot)
                    check("mapa pamieci ze znacznika Multiboot 2",
                          re.search(r"Pamiec fizyczna: \d+ KiB RAM",
                                    q21.wait_serial("Pamiec fizyczna",
                                                    timeout=20)) is not None)
                    check("initrd dotarl jako modul Multiboot 2",
                          re.search(r"initrd: \d+ plikow",
                                    q21.wait_serial("initrd:", timeout=20))
                          is not None)
                    check("wskaznik na tablice ACPI podany przez GRUB-a",
                          "ACPI: tablice od" in q21.wait_serial("ACPI:",
                                                                timeout=20))
                    q21.wait_serial("ProtonOS shell", timeout=30)
                    q21.type_line("uptime")
                    check("system z plyty odpowiada na komendy",
                          "uptime:" in q21.wait_serial("tyknac timera",
                                                       timeout=20))

                    # Konsola rysowana na buforze obrazu od GRUB-a:
                    # tryb tekstowy VGA w ogole tu nie dziala, wiec
                    # kazdy zapalony piksel to znak narysowany przez nas
                    w, h, at = q21.screen_raw()
                    check(f"loader ustawil tryb graficzny ({w}x{h})",
                          w >= 640 and h >= 400)
                    x0, y0 = (w - 640) // 2, (h - 200) // 2
                    lit = sum(1 for y in range(y0, y0 + 200)
                              for x in range(x0, x0 + 640)
                              if at(x, y) != (0, 0, 0))
                    check(f"konsola narysowana na buforze obrazu "
                          f"({lit} pikseli)", lit > 3000)
                    green = sum(1 for y in range(y0, y0 + 200)
                                for x in range(x0, x0 + 640)
                                if at(x, y) == (85, 255, 85))
                    check("kolory trybu tekstowego oddane w RGB "
                          f"(jasnozielone [ OK ]: {green})", green > 100)
                    outside = sum(1 for y in range(0, min(40, y0))
                                  for x in range(0, w, 4)
                                  if at(x, y) != (0, 0, 0))
                    check("poza blokiem tekstu ekran czysty", outside == 0)
                finally:
                    q21.close()
                    if os.path.exists(slog):
                        os.unlink(slog)

                # Pulpit na tym samym buforze: video_init_vesa nie ma
                # tu czego ustawiac, bo tryb ustawil juz loader
                with open(isodir + "/boot/grub/grub.cfg", "w") as fh:
                    fh.write("set timeout=0\nset default=0\n"
                             'menuentry "ProtonOS" {\n'
                             "    multiboot2 /boot/protonos.bin\n"
                             "    module2 /boot/initrd.img\n}\n")
                iso2 = tempfile.mktemp(suffix=".iso")
                try:
                    subprocess.run(["grub-mkrescue", "-o", iso2, isodir],
                                   stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
                    q22 = Qemu(KERNEL, INITRD, cdrom=iso2)
                    try:
                        time.sleep(22)
                        w, h, at = q22.screen_raw()
                        teal = sum(1 for y in range(0, h, 8)
                                   for x in range(0, w, 8)
                                   if all(abs(at(x, y)[i] - (0, 128, 160)[i])
                                          <= 40 for i in range(3)))
                        total = (h // 8) * (w // 8)
                        check(f"pulpit rysuje sie na buforze od loadera "
                              f"({w}x{h}, {teal}/{total} probek tla)",
                              teal > total * 0.7)
                    finally:
                        q22.close()
                finally:
                    if os.path.exists(iso2):
                        os.unlink(iso2)
        finally:
            shutil.rmtree(isodir, ignore_errors=True)
            if os.path.exists(iso):
                os.unlink(iso)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
