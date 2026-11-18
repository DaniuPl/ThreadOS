#!/usr/bin/env python3
"""ThreadOS smoke test: installing the applications' image.

The web engine's programs do not live in the initrd every boot carries
but in an image beside it, apps64.img, which the loader hands over as
the module "apps". A stick that brought one should leave a disk that
has one: otherwise the installed system starts without the web engine
the stick had, and nobody is told why.

The image is optional, and so is writing it. Where it fits it is the
fifth file in /EFI/ThreadOS; where it does not, the system is
installed without it and the plan says so before anything is written.
Three disks, one for each answer:

  - an EFI partition with plenty of room, installed beside: written;
  - an EFI partition nearly full, installed beside: left out, said,
    and the rest of the system written all the same;
  - the whole disk: the EFI partition made large enough to hold it,
    and the installed system, started on its own, running a program
    out of the image it now reads off its own disk.

What is on the disks afterwards is read by mtools, not asked of the
system that wrote it. The case of a whole disk too small for the
larger partition is in t_wholedisk.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import _kernel_define, _at, MTOOLS_ENV
from ptest import (check, summary, no64, make_gpt_disk, mtools_ok,
                   fat_format, fat_put, fat_get, gpt_read, stick_starts_live)
from mkblob import blob as pattern

ESP_MIB_APPS = _kernel_define("kernel/install.c", "ESP_MIB_APPS")
ESP_START = 2048

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
BUILD = os.path.dirname(os.path.abspath(KERNEL64 or "build/x"))
LOADER = os.path.join(BUILD, "BOOTX64.EFI")
ECHO = os.path.join(BUILD, "user64", "uecho64.elf")

# The program the image carries, under a name the initrd does not have:
# a program that runs under it can only have come out of the image
PROBE = "appsecho"


def blob(path):
    with open(path, "rb") as fh:
        return fh.read()


def make_apps(path):
    """An applications' image of the real format (scripts/mkinitrd.py):
    a program, and a megabyte of data that has to arrive bit for bit"""
    data = os.path.join(os.path.dirname(path), "pattern.bin")
    with open(data, "wb") as fh:
        fh.write(pattern("apps-image", 1 << 20))
    return subprocess.run([sys.executable,
                           os.path.join(ROOT, "scripts", "mkinitrd.py"),
                           path, f"{PROBE}={ECHO}", f"pattern.bin={data}"],
                          capture_output=True).returncode == 0


def make_stick(esp, apps):
    os.makedirs(os.path.join(esp, "EFI", "BOOT"))
    shutil.copy(LOADER, os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI"))
    shutil.copy(KERNEL64, os.path.join(esp, "threados64.bin"))
    shutil.copy(INITRD64, os.path.join(esp, "initrd64.img"))
    shutil.copy(apps, os.path.join(esp, "apps64.img"))
    stick_starts_live(esp)       # this group types at the shell


def fat_free(path, offset):
    """What mtools says is free on the filesystem, in bytes"""
    r = subprocess.run(["mdir", "-i", _at(path, offset), "::"],
                       env=MTOOLS_ENV, capture_output=True)
    m = re.search(r"([\d ]+) bytes free", r.stdout.decode(errors="replace"))
    return int(m.group(1).replace(" ", "")) if m else 0


def boot_stick(esp, disk, slog, nvram):
    shutil.copy(OVMF_VARS, nvram)
    q = Qemu(KERNEL, INITRD, esp=esp, disk=disk, sata=True, serial=slog,
             qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
    q.wait_serial("The 64-bit system is ready", timeout=120)
    return q


def beside(work, esp, apps, esp_mib, fill_to=None):
    """Installing beside a foreign system; 'fill_to' leaves that many
    bytes free on its EFI partition. The installer's output and the
    disk."""
    target = os.path.join(work, "beside%d.img" % esp_mib)
    make_gpt_disk(target, mib=esp_mib + 400, esp_mib=esp_mib)
    fat_format(target, ESP_START, esp_mib * 2048, fat32=True, label="OLD")
    if fill_to is not None:
        free = fat_free(target, ESP_START)
        if free > fill_to:
            fat_put(target, ESP_START, b"\0" * (free - fill_to),
                    "/somebody.bin")
    q = boot_stick(esp, target, os.path.join(work, "b%d.log" % esp_mib),
                   os.path.join(work, "vars.fd"))
    try:
        check("the stick's system reaches its desktop", q.wait_desktop())
        log = q.serial()
        m = re.search(r"Applications' image: (\d+) KiB", log)
        check("the stick's loader brought the applications' image",
              m is not None
              and int(m.group(1)) == os.path.getsize(apps) // 1024)
        plan = q.command("install text", "Type YES", timeout=40)
        done = q.command("YES", "Done", timeout=180)
    finally:
        q.close()
    return target, plan, done


def main():
    print()
    print("--- installing the applications' image ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    if not mtools_ok():
        print("SKIPPED: no mtools")
        return summary()
    if not os.path.exists(LOADER):
        subprocess.run(["make", "BUILD=" + BUILD, "efi"], cwd=ROOT,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not os.path.exists(LOADER):
        print("SKIPPED: no build/BOOTX64.EFI (apt install clang lld)")
        return summary()
    if not os.path.exists(OVMF_CODE) or not os.path.exists(OVMF_VARS):
        print("SKIPPED: no OVMF firmware")
        return summary()
    if not os.path.exists(ECHO):
        print("SKIPPED: no uecho64 - make")
        return summary()

    work = tempfile.mkdtemp()
    try:
        apps = os.path.join(work, "apps64.img")
        check("an applications' image with a program in it",
              make_apps(apps))
        want_apps = blob(apps)
        want_initrd = blob(INITRD64)
        esp = os.path.join(work, "stick")
        make_stick(esp, apps)

        # --- beside, with room: the image is the fifth file
        target, plan, done = beside(work, esp, apps, 600)
        check("the plan lists the image with its size",
              "/EFI/ThreadOS/apps64.img  %d B" % len(want_apps) in plan
              and "DOES NOT FIT" not in plan)
        check("and writes it after the initrd",
              "/EFI/ThreadOS/initrd64.img - done" in done
              and "/EFI/ThreadOS/apps64.img - done" in done
              and done.index("initrd64.img - done")
              < done.index("apps64.img - done"))
        got = fat_get(target, ESP_START, "/EFI/ThreadOS/apps64.img")
        check("mtools reads the image off the disk byte for byte (%d B)"
              % len(want_apps), got == want_apps)

        # --- beside, with too little room: the system without it.
        # Room for the system once and a half, which is enough to
        # install and not enough to keep an update's copy beside it
        # with the image in the way.
        need = (2 * os.path.getsize(LOADER) + os.path.getsize(KERNEL64)
                + len(want_initrd)) + 64 * 1024
        target, plan, done = beside(work, esp, apps, 300,
                                    fill_to=need * 3 // 2)
        check("on a nearly full EFI partition the plan says the image "
              "does not fit, before anything is written",
              "/EFI/ThreadOS/apps64.img  %d B  DOES NOT FIT" % len(want_apps)
              in plan and "too little room" in plan)
        check("the system is installed all the same",
              "/EFI/ThreadOS/initrd64.img - done" in done
              and "boot menu entry (UEFI) - done" in done)
        check("without the image", "apps64.img - done" not in done
              and fat_get(target, ESP_START,
                          "/EFI/ThreadOS/apps64.img") is None)
        check("and with the initrd whole",
              fat_get(target, ESP_START, "/EFI/ThreadOS/initrd64.img")
              == want_initrd)

        # --- the whole disk: an EFI partition that holds it
        target = os.path.join(work, "whole.img")
        with open(target, "wb") as fh:
            fh.truncate((ESP_MIB_APPS + 200) << 20)
        nvram = os.path.join(work, "whole.fd")
        q = boot_stick(esp, target, os.path.join(work, "w.log"), nvram)
        try:
            check("the stick's system reaches its desktop", q.wait_desktop())
            plan = q.command("install text whole", "Type ERASE", timeout=40)
            check("the plan makes an EFI partition of %d MiB for the image"
                  % ESP_MIB_APPS,
                  "an EFI partition of %d MiB (FAT32)" % ESP_MIB_APPS in plan
                  and "/EFI/ThreadOS/apps64.img  %d B" % len(want_apps)
                  in plan)
            done = q.command("ERASE", "Done", timeout=240)
            check("and writes the image onto it",
                  "/EFI/ThreadOS/apps64.img - done" in done)
        finally:
            q.close()
        ok, parts, why = gpt_read(target)
        check("the table holds together and its EFI partition is %d MiB"
              % ESP_MIB_APPS,
              ok and len(parts) == 2
              and parts[0][2] - parts[0][1] + 1 == ESP_MIB_APPS * 2048)
        got = fat_get(target, ESP_START, "/EFI/ThreadOS/apps64.img")
        check("mtools reads the image off it byte for byte",
              got == want_apps)

        # --- and the installed system, on its own, uses it
        q = Qemu(KERNEL, INITRD, disk=target, sata=True,
                 serial=os.path.join(work, "w2.log"),
                 qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            out = q.wait_serial("The 64-bit system is ready", timeout=120)
            m = re.search(r"Applications' image: (\d+) KiB", out)
            check("the installed loader hands the image over from its own "
                  "disk (%s)" % (m.group(0) if m else "nothing said"),
                  m is not None
                  and int(m.group(1)) == len(want_apps) // 1024)
            check("the installed system reaches its desktop",
                  q.wait_desktop())
            out = q.command("run %s off-the-installed-disk" % PROBE,
                            "exit code", timeout=60)
            check("and runs a program out of it",
                  "off-the-installed-disk" in out
                  and "[%s] done, exit code 0" % PROBE in out)
        finally:
            q.close()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
