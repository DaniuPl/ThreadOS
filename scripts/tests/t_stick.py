#!/usr/bin/env python3
"""ProtonOS smoke test: the stick a machine actually boots from.

What a UEFI machine starts from is not a disc and not a kernel: a disk
with a GPT, an EFI system partition in it, a FAT filesystem in that and
a loader at a name the firmware knows. `make stick` writes exactly
that, and this group checks the image the way the firmware will read it
- the table here in Python, with both CRCs, the filesystem through
mtools, the three files byte for byte - and then boots it.

And the two commands one types first on a machine nobody has tried
before: 'sprzet', which says what is in it, and 'dyski', which says
what it has to install onto. Both were missing from the 64-bit shell
until now, which is the kernel that will run on that machine.

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
LOADER = os.path.join(ROOT, "build", "BOOTX64.EFI")
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
        subprocess.run(["make", "efi"], cwd=ROOT, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
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
                           ("/protonos64.bin", want_kernel),
                           ("/initrd64.img", want_initrd)):
            got = fat_get(img, 2048, path)
            check("%s byte for byte (%d B)" % (path, len(want)),
                  got is not None and got == want)

        # --- and the only proof that counts
        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, disk=img, serial=slog,
                 qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            out = q.wait_serial("protoboot: ProtonOS", timeout=60)
            check("the firmware finds our loader on it", "protoboot" in out)
            out = q.wait_serial("handing the machine", timeout=30)
            check("the loader finds the kernel where we put it "
                  "(%d B)" % len(want_kernel),
                  "kernel: %d B" % len(want_kernel) in out)
            out = q.wait_serial("The 64-bit system is ready", timeout=120)
            check("and the system on the stick reaches the shell",
                  "Multiboot OK (Multiboot 2)" in out)

            # --- the two commands one types first on a strange machine
            out = q.command("hardware", "PCI:", timeout=30)
            check("'hardware' is in the 64-bit kernel - that is the one that will go there",
                  "Unknown command" not in out)
            check("and it says what screen the loader gave",
                  "screen:" in out and "od loadera" in out)
            check("what is on the PS/2 ports", "PS/2:" in out)
            check("co na USB", "USB:" in out)
            check("and what it sees on the PCI bus",
                  "PCI:" in out and "class" in out)

            out = q.command("disks", "disk 1", timeout=30)
            check("'disks' lists the machine's disks",
                  "disk 1:" in out and "in use" in out)
            check("and says what is on the one it uses",
                  "EFI (ESP)" in out)
        finally:
            q.close()
    finally:
        for f in (img, slog, nvram):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
