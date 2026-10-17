#!/usr/bin/env python3
"""Takes the two pictures in the README: the desktop and the installer.

    python3 scripts/screenshots.py

needs QEMU, grub-mkrescue, mtools, OVMF and Pillow, and a built 64-bit
system (make kernel64 efi). It boots the 64-bit kernel in QEMU, clicks
its way through the system the way the tests do - through the same
harness, scripts/ptest.py - and saves what QEMU's screen shows. So what
is in docs/ is what the kernel drew.

  docs/desktop.png    the desktop with a file manager, a clock and the
                      memory monitor open, the menu bar and the dock
  docs/installer.png  'install' on a UEFI machine, alone on the screen
"""

import os
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.argv = [sys.argv[0], os.path.join(ROOT, "build", "threados.bin"),
            os.path.join(ROOT, "build", "initrd.img"),
            os.path.join(ROOT, "build", "threados64.bin"),
            os.path.join(ROOT, "build", "initrd64.img")]
sys.path.insert(0, HERE)
from ptest import *          # noqa: F401,F403,E402
from ptest import make_iso64, make_gpt_disk, fat_format  # noqa: E402

from PIL import Image        # noqa: E402

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
LOADER = os.path.join(ROOT, "build", "BOOTX64.EFI")


def save(q, name):
    ppm = tempfile.mktemp(suffix=".ppm")
    q.cmd({"execute": "screendump", "arguments": {"filename": ppm}})
    Image.open(ppm).save(os.path.join(ROOT, "docs", name))
    os.unlink(ppm)
    print("docs/" + name)


def drag(q, x0, y0, x1, y1):
    q.mouse_goto(x0, y0)
    time.sleep(0.4)
    q.mouse_button(True)
    time.sleep(0.3)
    q.mouse_goto(x1, y1)
    time.sleep(0.5)
    q.mouse_button(False)
    time.sleep(1)


def desktop():
    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    try:
        make_iso64(d, iso, graphics=True)
        q = Qemu(KERNEL, INITRD, cdrom=iso, disk=disk,
                 qemu="qemu-system-x86_64",
                 serial=tempfile.mktemp(suffix=".log"))
        try:
            q.wait_serial("thread64>", timeout=120)
            for name in ("notes.txt", "report.txt", "todo.txt"):
                q.command("write %s hello" % name, "write")
            q.type_line("run mkdir documents")
            time.sleep(1)
            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=40)
            q.wait_desktop(40)
            q.mouse_home()
            w, h = q.size()
            # The kernel's two demonstration windows out of the way
            q.click(*light_xy(40, 60, 0))
            time.sleep(0.5)
            q.click(*light_xy(180, 95, 0))
            time.sleep(0.5)
            q.n_win = 0
            # The file manager, the monitor and the clock - placed. The
            # kernel's windows are gone, so they open in the first slots
            # of the cascade: (70,30), (82,42), (94,54)
            q.launch_dock(2)
            time.sleep(4)
            drag(q, 70 + 100, 30 + 4, 40 + 100, 50 + 4)
            q.launch_dock(3)
            time.sleep(4)
            drag(q, 82 + 120, 42 + 4, w - 300 + 120, 60 + 4)
            q.launch_dock(1)
            time.sleep(4)
            drag(q, 94 + 70, 54 + 4, w - 200 + 70, 210 + 4)
            # The pointer resting on a tile, so its name shows
            q.mouse_goto(*dock_tile(2, n_w=3, w=w, h=h))
            time.sleep(2)
            save(q, "desktop.png")
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, disk):
            if os.path.exists(f):
                os.unlink(f)


def installer():
    esp = tempfile.mkdtemp()
    target = tempfile.mktemp(suffix=".img")
    nvram = tempfile.mktemp(suffix=".fd")
    try:
        os.makedirs(os.path.join(esp, "EFI", "BOOT"))
        shutil.copy(LOADER, os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI"))
        shutil.copy(KERNEL64, os.path.join(esp, "threados64.bin"))
        shutil.copy(INITRD64, os.path.join(esp, "initrd64.img"))
        make_gpt_disk(target)
        fat_format(target, 2048, 204800, fat32=True, label="LAPTOPESP")
        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, esp=esp, disk=target, sata=True,
                 qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram),
                 serial=tempfile.mktemp(suffix=".log"))
        try:
            q.wait_serial("The 64-bit system is ready", timeout=120)
            q.type_line("install")
            time.sleep(10)
            q.mouse_goto(2000, 2000)
            time.sleep(1)
            save(q, "installer.png")
        finally:
            q.close()
    finally:
        shutil.rmtree(esp, ignore_errors=True)
        for f in (target, nvram):
            if os.path.exists(f):
                os.unlink(f)


desktop()
installer()
