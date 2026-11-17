#!/usr/bin/env python3
"""ThreadOS smoke test: the stick a machine actually boots from.

What a UEFI machine starts from is not a disc and not a kernel: a disk
with a GPT, an EFI system partition in it, a FAT filesystem in that and
a loader at a name the firmware knows. `make stick` writes exactly
that, and this group checks the image the way the firmware will read it
- the table here in Python, with both CRCs, the filesystem through
mtools, the three files byte for byte - and then boots it, to where a
person who boots a stick is going: the installer, on the whole screen,
with no menu and no desktop before it (threadboot hands the kernel
'install' when it is not the installed system's own loader).

Run on its own or through scripts/run_tests.py.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, no64, mtools_ok, fat_get, fat_dir,
                   fat_kind, gpt_read, GUID_ESP4)

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
# The loader of the same build as the kernel under test - which is not
# always build/ (make BUILD=...)
BUILD = os.path.dirname(os.path.abspath(KERNEL64))
LOADER = os.path.join(BUILD, "BOOTX64.EFI")
MKSTICK = os.path.join(ROOT, "scripts", "mkstick.py")


def blob(path):
    with open(path, "rb") as fh:
        return fh.read()


def main():
    print()
    print("--- the stick the machine boots from ---")

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
    if shutil.which("qemu-system-x86_64") is None:
        print("SKIPPED: no qemu-system-x86_64")
        return summary()

    img = tempfile.mktemp(suffix=".img")
    slog = tempfile.mktemp(suffix=".log")
    nvram = tempfile.mktemp(suffix=".fd")

    want_loader = blob(LOADER)
    want_kernel = blob(KERNEL64)
    want_initrd = blob(INITRD64)

    try:
        made = subprocess.run(
            [sys.executable, MKSTICK, img, LOADER, KERNEL64, INITRD64],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        check("the stick image builds with one command",
              made.returncode == 0 and os.path.exists(img))
        if made.returncode:
            print(made.stdout.decode(errors="replace"))
            return summary()
        check("and it says how to write it and what that erases",
              b"dd if=" in made.stdout and b"the WHOLE stick" in made.stdout)

        # --- the table, read the way a firmware reads one
        ok, parts, why = gpt_read(img)
        check("the GPT holds together (a protective MBR, both headers, "
              "both checksums)%s" % ("" if ok else ": " + why), ok)
        check("it has exactly one partition - EFI (%d)" % len(parts),
              len(parts) == 1 and parts[0][0] == GUID_ESP4)
        if parts:
            size = os.path.getsize(img) // 512
            check("it starts at 2048 and does not run into the table's copy",
                  parts[0][1] == 2048 and parts[0][2] <= size - 34)

        # --- the filesystem, read by somebody else
        check("the EFI partition holds a FAT32 (%s)" % fat_kind(img, 2048),
              fat_kind(img, 2048) == "FAT32")
        listing = fat_dir(img, 2048)
        check("mtools sees in it the directory the firmware looks in",
              "BOOT" in listing)
        for path, want in (("/EFI/BOOT/BOOTX64.EFI", want_loader),
                           ("/threados64.bin", want_kernel),
                           ("/initrd64.img", want_initrd)):
            got = fat_get(img, 2048, path)
            check("%s byte for byte (%d B)" % (path, len(want)),
                  got is not None and got == want)

        # --- and the only proof that counts
        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, disk=img, serial=slog,
                 qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            out = q.wait_serial("threadboot: ThreadOS", timeout=60)
            check("the firmware finds our loader on it", "threadboot" in out)
            out = q.wait_serial("handing the machine", timeout=30)
            check("the loader finds the kernel where we put it "
                  "(%d B)" % len(want_kernel),
                  "kernel: %d B" % len(want_kernel) in out)
            out = q.wait_serial("The 64-bit system is ready", timeout=120)
            check("and the system on the stick comes up",
                  "Multiboot OK (Multiboot 2)" in out)
            check("the loader asks for the installer, not a desktop",
                  "The installer is starting" in
                  q.wait_serial("The installer is starting", timeout=60)
                  and "The desktop is starting" not in q.serial())

            # The installer's own screen, read the way t_ginstal reads
            # it: no menu bar over the aurora, and its heading. Polled -
            # it is drawn once the program has started
            head, top, w = "", 0, 1
            deadline = time.time() + 60
            while time.time() < deadline:
                q.mouse_goto(900, 600)
                time.sleep(0.8)
                w, h, at = q.screen_raw()
                top = sum(1 for x in range(0, w, 8)
                          if at(x, 3) == aurora_px(x, 3, w, h))
                head = q.read_ui(14, ui_font_baseline_in(BAR_PX + 2, 8, 20),
                                 width=260, px=BAR_PX + 2, face="bold",
                                 fg=(255, 255, 255),
                                 under=lambda x, y: aurora_px(x, y, w, h))
                if "ThreadOS - installation" in head and top == w // 8:
                    break
            check("and it is on the whole screen ('%s', bare aurora %d/%d "
                  "along the top)" % (head, top, w // 8),
                  "ThreadOS - installation" in head and top == w // 8)
            check("nothing stopped it",
                  "was stopped" not in q.serial() and
                  "Invalid Opcode" not in q.serial())
        finally:
            q.close()
    finally:
        for f in (img, slog, nvram):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
