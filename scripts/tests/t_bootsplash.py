#!/usr/bin/env python3
"""The graphical boot path shows a splash and keeps its kernel log on serial.

The kernel is started by GRUB with 'desktop live' on its command line,
the arguments of the desktop entry in boot/grub64.cfg: that is the path
on which kernel/bootsplash.c takes the screen over. Only the arguments
are that entry's. The mode is the test's own, 640x480, set in its
gfxpayload so that the size checked below is known in advance; the
real entry leaves the mode to GRUB.
"""

import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64


def boot_splash(kernel, initrd, boot_args):
    work = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    log = tempfile.mktemp(suffix=".log")
    try:
        os.makedirs(os.path.join(work, "boot", "grub"))
        shutil.copy(kernel, os.path.join(work, "boot", "threados.bin"))
        module = ""
        if initrd and os.path.exists(initrd):
            shutil.copy(initrd, os.path.join(work, "boot", "initrd.img"))
            module = "    module2 /boot/initrd.img\n"
        # The mode is named in gfxpayload, the variable that decides what
        # the kernel is handed - and only AFTER the multiboot2 line:
        # loading a kernel whose header asks for any mode sets
        # gfxpayload=auto, and GRUB then picks 800x600 by itself
        cfg = (
            "insmod all_video\n"
            "set timeout=0\n"
            "set default=0\n"
            'menuentry "ThreadOS" {\n'
            f"    multiboot2 /boot/threados.bin {boot_args}\n"
            f"{module}"
            "    set gfxpayload=640x480x32\n"
            "}\n"
        )
        with open(os.path.join(work, "boot", "grub", "grub.cfg"), "w") as fh:
            fh.write(cfg)
        made = subprocess.run(
            ["grub-mkrescue", "-o", iso, work],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode == 0
        check("the boot ISO builds", made)
        if not made:
            return

        machine = Qemu(kernel, initrd, cdrom=iso, serial=log)
        try:
            serial = machine.wait_serial(
                "[boot] visual loading screen active", timeout=60)
            check("the kernel reports the graphical boot path",
                  "[boot] visual loading screen active" in serial)
            w, h, pixel = machine.screen_raw()
            # SPLASH_BG in kernel/bootsplash.c, in two opposite corners:
            # the text and the bar sit in the middle
            check("the splash fills the 640x480 framebuffer (%dx%d, %s and "
                  "%s in the corners)"
                  % (w, h, pixel(4, 4), pixel(w - 5, h - 5)),
                  w == 640 and h == 480 and pixel(4, 4) == (8, 17, 31) and
                  pixel(w - 5, h - 5) == (8, 17, 31))
        finally:
            machine.close()
    finally:
        shutil.rmtree(work, ignore_errors=True)
        for path in (iso, log):
            if os.path.exists(path):
                os.unlink(path)


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return 0

    boot_splash(KERNEL, INITRD, "desktop live")

    return summary()


if __name__ == "__main__":
    sys.exit(main())
