#!/usr/bin/env python3
"""ThreadOS smoke test: booting from a disc (Multiboot 2).

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
    # ACPI arrives as Multiboot 2 tags.
    print("--- booting off an ISO (Multiboot 2) ---")
    if shutil.which("grub-mkrescue") is None:
        print("SKIPPED: no grub-mkrescue "
              "(apt install grub-pc-bin grub-common xorriso)")
    else:
        isodir = tempfile.mkdtemp()
        iso = tempfile.mktemp(suffix=".iso")
        try:
            os.makedirs(isodir + "/boot/grub")
            shutil.copy(KERNEL, isodir + "/boot/threados.bin")
            shutil.copy(INITRD, isodir + "/boot/initrd.img")
            # A menu of our own: with no countdown and straight into text
            # mode, so that the screen can be read
            with open(isodir + "/boot/grub/grub.cfg", "w") as fh:
                fh.write("set timeout=0\nset default=0\n"
                         'menuentry "ThreadOS" {\n'
                         "    multiboot2 /boot/threados.bin nodesktop\n"
                         "    module2 /boot/initrd.img\n}\n")
            made = subprocess.run(["grub-mkrescue", "-o", iso, isodir],
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL).returncode == 0
            check("the ISO image builds", made)
            if made:
                slog = tempfile.mktemp(suffix=".log")
                q21 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog)
                try:
                    boot = q21.wait_serial("Multiboot OK", timeout=40)
                    check("the kernel loaded by GRUB speaks Multiboot 2",
                          "Multiboot OK (Multiboot 2)" in boot)
                    check("the memory map from the Multiboot 2 tag",
                          re.search(r"Physical memory: \d+ KiB RAM",
                                    q21.wait_serial("Physical memory",
                                                    timeout=20)) is not None)
                    check("the initrd arrived as a Multiboot 2 module",
                          re.search(r"initrd: \d+ files",
                                    q21.wait_serial("initrd:", timeout=20))
                          is not None)
                    check("the pointer to the ACPI tables handed over by GRUB",
                          "ACPI: tables from" in q21.wait_serial("ACPI:",
                                                                timeout=20))
                    q21.wait_serial("ThreadOS shell", timeout=30)
                    q21.type_line("uptime")
                    check("the system from the disc answers commands",
                          "uptime:" in q21.wait_serial("timer ticks",
                                                       timeout=20))

                    # The console drawn on the framebuffer from GRUB: VGA
                    # text mode does not work here at all, so every lit
                    # pixel is a character drawn by us
                    w, h, at = q21.screen_raw()
                    check(f"the loader set a graphics mode ({w}x{h})",
                          w >= 640 and h >= 400)
                    x0, y0 = (w - 640) // 2, (h - 200) // 2
                    lit = sum(1 for y in range(y0, y0 + 200)
                              for x in range(x0, x0 + 640)
                              if at(x, y) != (0, 0, 0))
                    check(f"the console is drawn on the framebuffer "
                          f"({lit} pixels)", lit > 3000)
                    green = sum(1 for y in range(y0, y0 + 200)
                                for x in range(x0, x0 + 640)
                                if at(x, y) == (85, 255, 85))
                    check("the text mode colours given back in RGB "
                          f"(a light green [ OK ]: {green})", green > 100)
                    outside = sum(1 for y in range(0, min(40, y0))
                                  for x in range(0, w, 4)
                                  if at(x, y) != (0, 0, 0))
                    check("outside the block of text the screen is clean", outside == 0)
                finally:
                    q21.close()
                    if os.path.exists(slog):
                        os.unlink(slog)

                # The desktop on the same buffer: video_init_vesa has
                # nothing to set here, because the loader has already set
                # the mode
                with open(isodir + "/boot/grub/grub.cfg", "w") as fh:
                    fh.write("set timeout=0\nset default=0\n"
                             'menuentry "ThreadOS" {\n'
                             "    multiboot2 /boot/threados.bin\n"
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
                        teal, total = aurora_samples(at, w, h)
                        check(f"the desktop draws on the loader's buffer "
                              f"({w}x{h}, {teal}/{total} background samples)",
                              teal > total * 0.7)

                        # A message printed while the desktop is
                        # running goes onto the VISIBLE buffer: the
                        # framebuffer console holds its address of its
                        # own and knows nothing about the compositor's
                        # working buffer. The next frame has to cover
                        # it. It is worth asking, because the
                        # compositor sends only the rows that changed -
                        # and rows somebody else wrote behind its back
                        # look unchanged to it.
                        # How soon the next frame comes is the
                        # compositor's business, and on a loaded host it
                        # is later than here. We look until the text is
                        # covered, rather than assuming three seconds.
                        q22.type_line("mem")
                        after = 0
                        want = teal - total // 100
                        for _ in range(10):
                            time.sleep(1.5)
                            w, h, at = q22.screen_raw()
                            after, _ = aurora_samples(at, w, h)
                            if after >= want:
                                break
                        check(f"and the kernel's text does not stay on the screen - "
                              f"the desktop covers it ({after}/{total} background "
                              f"samples, it was {teal})",
                              after >= want)
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
