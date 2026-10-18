#!/usr/bin/env python3
"""ThreadOS smoke test: installing from the disc (threados64.iso).

The stick image (make stick) starts through threadboot, which hands the
installer the files it writes. The disc starts through GRUB - and
people put a disc image on a stick with whatever program they have
(Rufus, Etcher, Ventoy, dd) rather than with ours. So the disc has to
be able to install too: it carries threadboot as a plain file, and
every one of its menu entries has GRUB hand over the kernel's file and
threadboot as modules, by the names threadboot itself gives them.

Checked from outside, on the disc 'make iso64' really builds:

  its GRUB menu is the one in grub64.cfg, and threadboot is on it byte
  for byte

  started under UEFI firmware (OVMF), with somebody else's disk behind
  an AHCI controller, 'install text' writes the three files - read back
  off the image by mtools and compared with the build's own - and
  leaves the other system's loader alone

  that disk then boots on its own, through threadboot, with no disc

  and the menu's install entry starts the installer by itself

Run on its own or through scripts/run_tests.py.
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
from ptest import (check, summary, no64, make_gpt_disk, gpt_intact,
                   mtools_ok, fat_format, fat_mkdirs, fat_put, fat_get)

ESP_START = 2048
ESP_SECTORS = 204800
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
ISO = os.path.join(ROOT, "build", "threados64.iso")
LOADER = os.path.join(ROOT, "build", "BOOTX64.EFI")
FOREIGN_BOOTMGR = b"MZ-this-is-somebody-elses-bootmgfw-" * 300


def blob(path):
    with open(path, "rb") as fh:
        return fh.read()


def iso_file(iso, path, out):
    """A file off the disc, by xorriso - somebody else's reading of it"""
    r = subprocess.run(["xorriso", "-osirrox", "on", "-indev", iso,
                        "-extract", path, out],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return blob(out) if r.returncode == 0 and os.path.exists(out) else None


def remaster(iso, work, default):
    """The same disc with another menu entry chosen and no wait: the
    tree off the disc, its grub.cfg with only 'default' changed"""
    tree = os.path.join(work, "tree%d" % default)
    subprocess.run(["xorriso", "-osirrox", "on", "-indev", iso,
                    "-extract", "/boot", os.path.join(tree, "boot")],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    cfg = os.path.join(tree, "boot", "grub", "grub.cfg")
    os.chmod(cfg, 0o644)
    text = blob(cfg).decode()
    text = text.replace("set default=0", "set default=%d" % default)
    text = text.replace("set timeout=2", "set timeout=0")
    with open(cfg, "w") as fh:
        fh.write(text)
    for root, dirs, files in os.walk(tree):
        for n in dirs + files:
            os.chmod(os.path.join(root, n), 0o755)
    out = os.path.join(work, "menu%d.iso" % default)
    r = subprocess.run(["grub-mkrescue", "-o", out, tree],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return out if r.returncode == 0 else None


def make_target(path):
    """The other machine's disk: a GPT, an EFI partition, Windows' loader"""
    make_gpt_disk(path)
    if not fat_format(path, ESP_START, ESP_SECTORS, fat32=True,
                      label="LAPTOPESP"):
        return False
    fat_mkdirs(path, ESP_START, ["/EFI", "/EFI/Microsoft",
                                 "/EFI/Microsoft/Boot"])
    fat_put(path, ESP_START, FOREIGN_BOOTMGR,
            "/EFI/Microsoft/Boot/bootmgfw.efi")
    return True


def main():
    print()
    print("--- installing from the disc ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    for tool in ("xorriso", "grub-mkrescue"):
        if shutil.which(tool) is None:
            print("SKIPPED: no " + tool)
            return summary()
    if not mtools_ok():
        print("SKIPPED: no mtools")
        return summary()
    if not os.path.exists(OVMF_CODE) or not os.path.exists(OVMF_VARS):
        print("SKIPPED: no OVMF firmware")
        return summary()
    if not os.path.exists(ISO) or not os.path.exists(LOADER):
        print("SKIPPED: no build/threados64.iso or build/BOOTX64.EFI "
              "(make iso64 efi)")
        return summary()

    work = tempfile.mkdtemp()
    target = os.path.join(work, "target.img")
    nvram = os.path.join(work, "vars.fd")
    want_loader = blob(LOADER)
    want_kernel = blob(KERNEL64)
    want_initrd = blob(INITRD64)
    try:
        # --- The disc itself
        cfg = iso_file(ISO, "/boot/grub/grub.cfg", os.path.join(work, "g.cfg"))
        check("the disc's GRUB menu is grub64.cfg",
              cfg is not None and cfg == blob(os.path.join(ROOT, "grub64.cfg")))
        entries = re.findall(r'menuentry "([^"]+)" \{(.*?)\}',
                             (cfg or b"").decode(), re.S)
        check(f"every one of its {len(entries)} entries hands over the "
              "kernel's file and threadboot, by the names the installer "
              "looks for",
              len(entries) == 3 and all(
                  "module2 /boot/threados64.bin the kernel" in body
                  and "module2 /boot/threadboot.efi loader" in body
                  for _, body in entries))
        check("one of them is the installer",
              any(" install\n" in body for _, body in entries))
        got = iso_file(ISO, "/boot/threadboot.efi", os.path.join(work, "l.efi"))
        check(f"threadboot is on the disc byte for byte ({len(want_loader)} B)",
              got == want_loader)

        shell_iso = remaster(ISO, work, 2)
        install_iso = remaster(ISO, work, 1)
        check("(the same disc, with the shell or the installer chosen)",
              shell_iso is not None and install_iso is not None)
        if not shell_iso or not install_iso:
            return summary()

        # --- Installing in text, from the disc
        check("somebody else's disk is ready (GPT + FAT32 from mtools)",
              make_target(target))
        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, cdrom=shell_iso, disk=target, sata=True,
                 serial=os.path.join(work, "s1.log"),
                 qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            q.wait_serial("The 64-bit system is ready", timeout=120)
            out = q.command("install text", "Type YES", timeout=30)
            check("the installer started from GRUB has something to install "
                  "and says what it would write",
                  "/EFI/ThreadOS/threados64.bin" in out
                  and "nothing to install" not in out)
            out = q.command("YES", "Done", timeout=90)
            check("after YES it writes the three files and the fallback",
                  all(f"{p} - done" in out for p in (
                      "/EFI/ThreadOS/BOOTX64.EFI",
                      "/EFI/ThreadOS/threados64.bin",
                      "/EFI/ThreadOS/initrd64.img",
                      "/EFI/BOOT/BOOTX64.EFI")))
        finally:
            q.close()

        for path, want in (("/EFI/ThreadOS/BOOTX64.EFI", want_loader),
                           ("/EFI/ThreadOS/threados64.bin", want_kernel),
                           ("/EFI/ThreadOS/initrd64.img", want_initrd),
                           ("/EFI/BOOT/BOOTX64.EFI", want_loader)):
            got = fat_get(target, ESP_START, path)
            check(f"{path} byte for byte, as mtools reads it ({len(want)} B)",
                  got is not None and got == want)
        check("Windows' loader is untouched",
              fat_get(target, ESP_START, "/EFI/Microsoft/Boot/bootmgfw.efi")
              == FOREIGN_BOOTMGR)
        check("and so is the partition table", gpt_intact(target))

        # --- The disk on its own, with no disc
        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, disk=target, sata=True,
                 serial=os.path.join(work, "s2.log"),
                 qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            out = q.wait_serial("threadboot: ThreadOS", timeout=60)
            check("the installed disk boots on its own, through threadboot",
                  "threadboot" in out)
            out = q.wait_serial("The 64-bit system is ready", timeout=90)
            check("and reaches the system", "Multiboot OK" in out)
        finally:
            q.close()

        # --- The menu's own install entry
        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, cdrom=install_iso,
                 disk=os.path.join(work, "target.img"), sata=True,
                 serial=os.path.join(work, "s3.log"),
                 qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            q.wait_serial("The installer is starting", timeout=120)
            time.sleep(10)
            q.mouse_goto(900, 600)
            time.sleep(0.8)
            # The installer's own screen, read the way t_ginstal reads
            # it: no menu bar over the aurora, and its heading
            w, h, at = q.screen_raw()
            top = sum(1 for x in range(0, w, 8)
                      if at(x, 3) == aurora_px(x, 3, w, h))
            head = q.read_ui(14, ui_font_baseline_in(BAR_PX + 2, 8, 20),
                             width=260, px=BAR_PX + 2, face="bold",
                             fg=(255, 255, 255),
                             under=lambda x, y: aurora_px(x, y, w, h))
            check("the install entry starts the installer by itself, on the "
                  f"whole screen ('{head}', bare aurora {top}/{w // 8} "
                  "along the top)",
                  "ThreadOS - installation" in head and top == w // 8)
        finally:
            q.close()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
