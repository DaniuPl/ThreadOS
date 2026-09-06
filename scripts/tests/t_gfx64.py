#!/usr/bin/env python3
"""ProtonOS smoke test: graphics mode in the 64-bit kernel.

The check drawing is simple on purpose: four rectangles in the corners,
a frame and a caption in our own font. Every error in computing a
pixel's address shows on it at once - a wrong bytes-per-pixel skews the
image, and swapped channels can be seen in the colours.

We check under BIOS and under UEFI, because it is the firmware that
decides how many bits a pixel has, and 24 bits is the case in which a
whole-word write tramples the neighbour.
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

# video.c's palette colours: 1 red, 2 green, 3 blue, 4 white, 5 yellow
RED = (255, 0, 0)
GREEN = (0, 255, 0)
BLUE = (0, 0, 255)
BIALY = (255, 255, 255)
YELLOW = (255, 255, 0)


def near(a, b, tol=30):
    return all(abs(a[i] - b[i]) <= tol for i in range(3))


def inspect(iso, via_uefi):
    """Draws the pattern and checks where the colours landed."""
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
        out = q.wait_serial("gfx: drawing", timeout=30)
        m = re.search(r"gfx: drawing (\d+)x(\d+), (\d+) bits", out)
        check(f"{label}: the kernel draws on the firmware's buffer"
              + (f" ({m.group(1)}x{m.group(2)}, {m.group(3)} b/px)"
                 if m else ""),
              m is not None)
        if not m:
            return
        time.sleep(1.5)

        w, h, at = q.screen_raw()
        # The corners: each a different colour, so a mistake in a pixel's
        # address swaps them around or smears them
        check(f"{label}: the top left corner is red",
              near(at(w // 8, h // 8), RED))
        check(f"{label}: the top right corner is green",
              near(at(w - w // 8, h // 8), GREEN))
        check(f"{label}: the bottom left corner is yellow",
              near(at(w // 8, h - h // 8), YELLOW))
        check(f"{label}: the bottom right corner is white",
              near(at(w - w // 8, h - h // 8), BIALY))
        check(f"{label}: the background between the corners is blue",
              near(at(w // 2, h // 8), BLUE))

        # The caption in our own font: we count the bright pixels in its
        # band. A wrong bytes-per-pixel would leave rubbish or emptiness
        # there.
        pas = sum(1 for y in range(h // 2, h // 2 + 8)
                  for x in range(w // 2 - 120, w // 2 + 120)
                  if near(at(x, y), BIALY))
        check(f"{label}: text drawn in our own font "
              f"({pas} pixels)", 100 < pas < 1500)
    finally:
        q.close()
        for f in (slog, nvram):
            if os.path.exists(f):
                os.unlink(f)


def main():
    print("--- graphics mode (the 64-bit kernel) ---")

    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("SKIPPED: no 64-bit image (make kernel64)")
        return 0
    if shutil.which("grub-mkrescue") is None:
        print("SKIPPED: no grub-mkrescue")
        return 0
    if shutil.which("qemu-system-x86_64") is None:
        print("SKIPPED: no qemu-system-x86_64")
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
        check("the disc with the 64-bit kernel builds", made)
        if not made:
            return summary()

        inspect(iso, False)

        if (os.path.exists(OVMF_CODE) and os.path.exists(OVMF_VARS)
                and os.path.isdir("/usr/lib/grub/x86_64-efi")
                and shutil.which("mformat")):
            inspect(iso, True)
        else:
            print("SKIPPED: no OVMF or no GRUB for EFI")
    finally:
        shutil.rmtree(d, ignore_errors=True)
        if os.path.exists(iso):
            os.unlink(iso)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
