#!/usr/bin/env python3
"""The graphical boot path shows a splash and keeps its kernel log on serial."""

import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary


def boot_splash(kernel, initrd, boot_args, qemu="qemu-system-i386"):
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
        cfg = (
            "insmod all_video\n"
            "set gfxmode=640x480x32\n"
            "set gfxpayload=keep\n"
            "set timeout=0\n"
            "set default=0\n"
            'menuentry "ThreadOS" {\n'
            f"    multiboot2 /boot/threados.bin {boot_args}\n"
            f"{module}"
            "}\n"
        )
        with open(os.path.join(work, "boot", "grub", "grub.cfg"), "w") as fh:
            fh.write(cfg)
        made = subprocess.run(
            ["grub-mkrescue", "-o", iso, work],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode == 0
        check(f"the boot ISO builds for {qemu}", made)
        if not made:
            return

        machine = Qemu(kernel, initrd, cdrom=iso, serial=log, qemu=qemu)
        try:
            serial = machine.wait_serial(
                "[boot] visual loading screen active", timeout=60)
            check(f"{qemu}: the kernel reports the graphical boot path",
                  "[boot] visual loading screen active" in serial)
            w, h, pixel = machine.screen_raw()
            check(f"{qemu}: the splash fills the 640x480 framebuffer",
                  w == 640 and h == 480 and pixel(4, 4) == (8, 17, 31))
        finally:
            machine.close()
    finally:
        shutil.rmtree(work, ignore_errors=True)
        for path in (iso, log):
            if os.path.exists(path):
                os.unlink(path)


def main():
    if shutil.which("grub-mkrescue") is None:
        print("SKIPPED: no grub-mkrescue")
        return 0

    if KERNEL and os.path.exists(KERNEL):
        boot_splash(KERNEL, INITRD, "")

    if KERNEL64 and os.path.exists(KERNEL64) and \
            shutil.which("qemu-system-x86_64"):
        boot_splash(KERNEL64, INITRD64, "desktop live",
                    qemu="qemu-system-x86_64")

    return summary()


if __name__ == "__main__":
    sys.exit(main())
