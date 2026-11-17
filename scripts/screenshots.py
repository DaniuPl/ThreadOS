#!/usr/bin/env python3
"""Takes the pictures in the README, every one on a 1920x1080 screen.

    python3 scripts/screenshots.py [BUILD [boot|desktop|installer|sharp|devices ...]]

needs QEMU, grub-mkrescue, mtools, OVMF and Pillow, and a built 64-bit
system (make, make efi) in BUILD (build/ unless given). It boots the
kernel in QEMU, clicks its way through the system the way the tests do -
through the same harness, scripts/ptest.py - and saves what QEMU's
screen shows. So what is in docs/ is what the kernel drew. The screen
says it is a 1920x1080 panel (its EDID), and the system takes it at its
own size and at one and a half, as it would a laptop's.

  docs/boot.png       the loading screen: the mark and the bar
  docs/desktop.png    the desktop with a file manager, a clock and the
                      memory monitor open, the menu bar and the dock
  docs/installer.png  'install' on a UEFI machine, alone on the screen
  docs/sharp.png      the calendar, the calculator and the menu
  docs/devices.png    Settings' Device Manager on a machine built as a
                      laptop is: a network card behind a bridge, two
                      sound controllers
"""

import os
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build")
WANTED = sys.argv[2:]                  # the pictures to take; all without
sys.argv = [sys.argv[0], os.path.join(BUILD, "threados64.bin"),
            os.path.join(BUILD, "initrd64.img")]
sys.path.insert(0, HERE)
from ptest import *          # noqa: F401,F403,E402
from ptest import make_iso64, make_gpt_disk, fat_format  # noqa: E402

from PIL import Image        # noqa: E402

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
LOADER = os.path.join(BUILD, "BOOTX64.EFI")
FULL_HD = (1920, 1080)


def save(q, name):
    ppm = tempfile.mktemp(suffix=".ppm")
    q.cmd({"execute": "screendump", "arguments": {"filename": ppm}})
    Image.open(ppm).save(os.path.join(ROOT, "docs", name))
    os.unlink(ppm)
    print("docs/" + name)


def opened(q, x, y, timeout=40):
    """Waits for a window to stand at (x, y), its top left corner: the
    pixel just inside its title bar no longer the aurora's. At 1920x1080
    QEMU draws several times slower than at 800x600, and a window opens
    seconds after it is asked for."""
    end = time.time() + timeout
    while time.time() < end:
        w, h, at = q.screen_raw()
        if at(x + 30, y + 4) != aurora_px(x + 30, y + 4, w, h):
            time.sleep(1)
            return True
        time.sleep(0.5)
    return False


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
        make_iso64(d, iso, graphics=True, gfxmode="1920x1080")
        q = Qemu(KERNEL, INITRD, cdrom=iso, disk=disk, edid=FULL_HD,
                 qemu="qemu-system-x86_64", ram_mib=512,
                 serial=tempfile.mktemp(suffix=".log"))
        try:
            q.wait_serial("thread64>", timeout=120)
            for name in ("notes.txt", "report.txt", "todo.txt"):
                q.command("write %s hello" % name, "write")
            # Folders, so the file manager's tree has something to show
            for folder in ("documents", "documents/photos", "projects"):
                q.type_line("mkdir " + folder)
                time.sleep(0.5)
            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=40)
            q.wait_desktop()
            q.mouse_home()
            w, h = q.size()
            q.n_win = 0
            # The file manager, the monitor and the clock - placed. They
            # open in the first slots of the cascade: (70,30), (82,42),
            # (94,54)
            q.launch_dock("gfiles")
            opened(q, 70, 30)
            drag(q, 70 + 100, 30 + 4, 16 + 100, 44 + 4)
            q.launch_dock("gmon")
            opened(q, 82, 42)
            drag(q, 82 + 120, 42 + 4, w - 234 + 120, 44 + 4)
            q.launch_menu("gclock")
            opened(q, 94, 54)
            drag(q, 94 + 70, 54 + 4, w - 150 + 70, 200 + 4)
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
                 edid=FULL_HD, ram_mib=512,
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


def sharp():
    """The calendar, the calculator and the menu on a 1920x1080 panel that
    says nothing about itself: its height gives one and a half"""
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    try:
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, ram_mib=512,
                 serial=tempfile.mktemp(suffix=".log"))
        try:
            q.wait_serial("thread64>", timeout=90)
            q.sudo("set resolution 1920x1080", "saved")
            q.type_line("desktop")
            # sudo made the administrator known: the desktop asks for the
            # password, which the field it starts in takes
            time.sleep(15)
            q.type_line("thread")
            q.wait_desktop()
            q.mouse_home()
            q.n_win = 0
            # The calendar and the calculator, side by side
            q.launch_menu("gcal")
            opened(q, 70, 30)
            drag(q, 70 + 100, 30 + 4, 330 + 100, 90 + 4)
            q.launch_menu("gcalc")
            opened(q, 82, 42)
            drag(q, 82 + 100, 42 + 4, 680 + 100, 90 + 4)
            q.open_menu()
            q.mouse_goto(60, session_y("About ThreadOS"))
            time.sleep(2)
            save(q, "sharp.png")
        finally:
            q.close()
    finally:
        if os.path.exists(disk):
            os.unlink(disk)


def boot():
    """The loading screen, half way: of the pictures taken while it is up,
    the one whose bar is the middle length. Started as a stick starts on
    a UEFI machine - threadboot, which sets the panel's own mode (its
    EDID's), the mode the loading screen is drawn in - with 'desktop'
    for its command line (\\cmdline.txt)"""
    esp = tempfile.mkdtemp()
    nvram = tempfile.mktemp(suffix=".fd")
    try:
        os.makedirs(os.path.join(esp, "EFI", "BOOT"))
        shutil.copy(LOADER, os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI"))
        shutil.copy(KERNEL64, os.path.join(esp, "threados64.bin"))
        shutil.copy(INITRD64, os.path.join(esp, "initrd64.img"))
        with open(os.path.join(esp, "cmdline.txt"), "w") as f:
            f.write("desktop\n")
        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, esp=esp, uefi=(OVMF_CODE, nvram),
                 edid=FULL_HD, ram_mib=512, qemu="qemu-system-x86_64",
                 serial=tempfile.mktemp(suffix=".log"))
        try:
            shots = []
            end = time.time() + 120
            while time.time() < end:
                ppm = tempfile.mktemp(suffix=".ppm")
                try:
                    q.cmd({"execute": "screendump",
                           "arguments": {"filename": ppm}})
                    im = Image.open(ppm).convert("RGB")
                    im.load()
                except Exception:
                    time.sleep(0.2)
                    continue
                finally:
                    if os.path.exists(ppm):
                        os.unlink(ppm)
                w, h = im.size
                px = im.load()
                black = w >= 1280 and all(
                    px[x, y] == (0, 0, 0) for x, y in
                    ((2, 2), (w - 3, 2), (2, h - 3), (w - 3, h - 3)))
                lit = black and sum(px[x, h // 2 - 40] != (0, 0, 0)
                                    for x in range(w // 2 - 80, w // 2 + 80))
                if lit:
                    bar = sum(px[x, y] == (255, 255, 255)
                              for y in range(h // 2 + 50, h // 2 + 160)
                              for x in range(w // 2 - 200, w // 2 + 200, 2))
                    shots.append((bar, im))
                elif shots:
                    break                  # the desktop took its place
                time.sleep(0.1)
            if shots:
                shots.sort(key=lambda t: t[0])
                shots[len(shots) // 2][1].save(os.path.join(ROOT, "docs",
                                                            "boot.png"))
                print("docs/boot.png")
        finally:
            q.close()
    finally:
        shutil.rmtree(esp, ignore_errors=True)
        if os.path.exists(nvram):
            os.unlink(nvram)


def devices():
    """Settings' Device Manager on a machine built as a laptop is: the
    network card on a bus of its own behind a bridge, and two HD Audio
    controllers - the first with no codec, the second with one"""
    wav = tempfile.mktemp(suffix=".wav")
    q = Qemu(KERNEL, INITRD, nic=None, edid=FULL_HD, ram_mib=512,
             usbkbd=True, serial=tempfile.mktemp(suffix=".log"),
             devices=["-device", "pci-bridge,id=br1,chassis_nr=1,addr=0x10",
                      "-device", "e1000,netdev=n0,bus=br1,addr=0x1",
                      "-audiodev", "wav,id=snd0,path=" + wav,
                      "-device", "intel-hda,id=hda0,addr=0xe",
                      "-device", "intel-hda,id=hda1,addr=0xf",
                      "-device", "hda-output,bus=hda1.0,audiodev=snd0"])
    try:
        q.wait_desktop()
        w, h = q.size()
        q.mouse_home()
        q.launch_dock("gsettings")
        at = None
        end = time.time() + 20
        while time.time() < end and at is None:
            time.sleep(0.5)
            for slot in range(8):
                x, y = cascade(slot, GSET_W + 2, GSET_H + TITLE_H + 2, w=w, h=h)
                if q.window_title(x, y, GSET_W + 2) == "Settings":
                    at = (x + 1, y + CONTENT_DY)
                    break
        if at:
            cx, cy = at
            k = GSET_SECTIONS.index("Devices")
            q.click(cx + GSET_SIDE_W // 2,
                    cy + GSET_SIDE_Y0 + k * GSET_SIDE_H + GSET_SIDE_H // 2)
            q.mouse_goto(w - 20, h // 2)
            time.sleep(3)
            save(q, "devices.png")
    finally:
        q.close()
        if os.path.exists(wav):
            os.unlink(wav)


SHOTS = {"boot": boot, "desktop": desktop, "installer": installer,
         "sharp": sharp, "devices": devices}
for name in WANTED or list(SHOTS):
    SHOTS[name]()
