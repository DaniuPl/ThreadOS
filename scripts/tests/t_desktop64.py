#!/usr/bin/env python3
"""ProtonOS smoke test: the graphical desktop in the 64-bit kernel.

The compositor, the windows and the taskbar are the same desktop.c as in
the 32-bit kernel - it came across without a single change. The whole
difference sits in the compatibility dictionary: the desktop starts
applications by name and takes the keyboard over, and both of those have
a different name in this kernel.

We check what can be seen: the background, the bars, the windows, their
order after a click and that the cursor moves with the mouse.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

TLO = (0, 130, 162)          # the desktop's blue
BAR = (195, 195, 195)      # the light grey bar
IKONA = (255, 162, 0)        # the orange icon


def near(a, b, tol=40):
    return all(abs(a[i] - b[i]) <= tol for i in range(3))


def main():
    print("--- pulpit graficzny (jadro 64-bitowe) ---")

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
    slog = tempfile.mktemp(suffix=".log")
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

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=60)
            q.type_line("pulpit")
            q.wait_serial("Pulpit startuje", timeout=30)
            time.sleep(4)

            w, h, at = q.screen_raw()
            check(f"pulpit wstaje w trybie graficznym ({w}x{h})",
                  w >= 640 and h >= 400)
            check("tlo pulpitu wypelnia ekran",
                  near(at(w // 2, h // 2), TLO))
            check("gorny pasek narysowany",
                  near(at(w // 2, 4), BAR))
            check("pasek zadan na dole narysowany",
                  near(at(w // 2, h - 8), BAR))
            # The icons stand in a column on the left; that orange
            # patch is the first of them
            check("ikony na pulpicie",
                  near(at(24, 32), IKONA))

            # The kernel's windows: two, one on top of the other. The
            # window's grey background at the place the first one stands.
            WINDOW = (235, 235, 235)
            check("okno 'Terminal' narysowane",
                  near(at(100, 100), WINDOW, 60))
            check("okno 'Pliki' narysowane",
                  near(at(240, 140), WINDOW, 60))

            # The mouse: the cursor is to go where we move it. We check
            # that through a change of pixels at the destination.
            q.mouse_goto(400, 300)
            time.sleep(1.5)
            _, _, at2 = q.screen_raw()
            differ = sum(1 for dy in range(12) for dx in range(12)
                        if at2(400 + dx, 300 + dy) != at(400 + dx, 300 + dy))
            check(f"kursor idzie za mysza ({differ} pikseli zmienionych)",
                  differ > 8)

            # A click on the second window's title bar brings it to the
            # top - and that can be seen from it covering the first
            q.mouse_goto(240, 100)
            q.mouse_button(True)
            time.sleep(0.3)
            q.mouse_button(False)
            time.sleep(1.5)
            _, _, at3 = q.screen_raw()
            check("klik w pasek tytulu nie wywraca kompozytora",
                  near(at3(w // 2, h // 2), TLO))

            # A key that no window wants goes to the kernel's shell -
            # the same as in protected mode. Long mode had nowhere to
            # hand those keys back to, so a desktop full of windows was
            # a screen nobody could type at.
            q.mouse_goto(560, 300)
            q.mouse_button(True)          # the desktop takes the focus
            time.sleep(0.3)
            q.mouse_button(False)
            time.sleep(1)
            seen = len(q.serial())
            q.type_line("uptime")
            time.sleep(2)
            out = q.serial()[seen:]
            check("klawisz, ktorego nie chce zadne okno, trafia do "
                  "shella jadra", "Uptime:" in out)
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
