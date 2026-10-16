#!/usr/bin/env python3
"""ThreadOS smoke test: installing onto the machine's own disk (in text).

The installer proper takes the whole screen and is checked in t_ginstal;
this group is the same three files written by the same code, said out
loud in the console - which is what one wants when there is no screen
to draw on, or when the answer has to end up in a log.

The shape of this group is the shape of the thing: a machine started
from a stick, a second disk that belongs to somebody else, and the
question of whether what lands on it is right and whether what was
already there is still there.

Nothing here trusts the system's own account of what it did. Every file
it writes is read back off the image by mtools and compared byte for
byte, and at the end the target disk is booted on its own - which is
the only proof that installing installed anything.

Run on its own or through scripts/run_tests.py.
"""

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, no64, make_gpt_disk, gpt_intact,
                   neighbours_intact, mtools_ok, fat_format, fat_mkdirs,
                   fat_put, fat_get, fat_dir)

ESP_START = 2048
ESP_SECTORS = 204800

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
LOADER = os.path.join(ROOT, "build", "BOOTX64.EFI")

FOREIGN_BOOTMGR = b"MZ-this-is-somebody-elses-bootmgfw-" * 300
FOREIGN_FALLBACK = b"MZ-somebody-elses-loader-in-EFI-BOOT-" * 200


def blob(path):
    with open(path, "rb") as fh:
        return fh.read()


def make_target(path, with_fallback):
    """The other machine's disk: a table, an EFI partition with a
    filesystem somebody else made, and somebody else's boot loader in
    it."""
    make_gpt_disk(path)
    if not fat_format(path, ESP_START, ESP_SECTORS, fat32=True,
                      label="LAPTOPESP"):
        return False
    fat_mkdirs(path, ESP_START, ["/EFI", "/EFI/Microsoft",
                                 "/EFI/Microsoft/Boot"])
    fat_put(path, ESP_START, FOREIGN_BOOTMGR,
            "/EFI/Microsoft/Boot/bootmgfw.efi")
    if with_fallback:
        fat_mkdirs(path, ESP_START, ["/EFI/BOOT"])
        fat_put(path, ESP_START, FOREIGN_FALLBACK, "/EFI/BOOT/BOOTX64.EFI")
    return True


def boot_stick(where):
    """The stick the machine is started from: our loader and the two
    files it reads."""
    os.makedirs(os.path.join(where, "EFI", "BOOT"), exist_ok=True)
    shutil.copy(LOADER, os.path.join(where, "EFI", "BOOT", "BOOTX64.EFI"))
    shutil.copy(KERNEL64, os.path.join(where, "threados64.bin"))
    shutil.copy(INITRD64, os.path.join(where, "initrd64.img"))


def main():
    print()
    print("--- installing onto the machine's disk ---")

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

    esp = tempfile.mkdtemp()
    target = tempfile.mktemp(suffix=".img")
    taken = tempfile.mktemp(suffix=".img")
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")
    slog3 = tempfile.mktemp(suffix=".log")
    nvram = tempfile.mktemp(suffix=".fd")

    want_loader = blob(LOADER)
    want_kernel = blob(KERNEL64)
    want_initrd = blob(INITRD64)

    try:
        boot_stick(esp)
        check("the foreign machine's disk is ready (GPT + FAT32 from mtools)",
              make_target(target, with_fallback=False))

        # --- A machine started from the stick, with the other one's
        # disk behind an AHCI controller - which is the disk the system
        # picks, and which is exactly the situation being tested
        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, esp=esp, disk=target, sata=True,
                 serial=slog, qemu="qemu-system-x86_64",
                 uefi=(OVMF_CODE, nvram))
        try:
            out = q.wait_serial("threadboot: ThreadOS", timeout=60)
            check("the machine boots off the stick through our loader",
                  "threadboot" in out)
            head = q.wait_serial("loader:", timeout=30)
            check("the loader hands over the kernel and ITSELF as modules - without that "
                  "there is nothing to install from",
                  f"loader: {len(want_loader)} B" in head)
            q.wait_serial("The 64-bit system is ready", timeout=90)

            # Somebody who does not type the word gets nothing done
            out = q.command("install text", "Type YES", timeout=30)
            check("the installer first says what it would write",
                  "/EFI/ThreadOS/threados64.bin" in out
                  and "EFI partition: number 1" in out)
            check("and it promises what it will not touch",
                  "I do NOT touch: the partition table" in out)
            out = q.command("no", "install:", timeout=20)
            check("without the word YES it writes nothing",
                  "nothing was written" in out)

            out = q.command("install text", "Type YES", timeout=30)
            out = q.command("YES", "Done", timeout=90)
            check("after YES it writes all three files",
                  "/EFI/ThreadOS/BOOTX64.EFI - done" in out
                  and "/EFI/ThreadOS/threados64.bin - done" in out
                  and "/EFI/ThreadOS/initrd64.img - done" in out)
            check("and - since nobody was there - "
                  "/EFI/BOOT/BOOTX64.EFI too",
                  "/EFI/BOOT/BOOTX64.EFI - done" in out
                  and "will boot ThreadOS off the disk" in out)
        finally:
            q.close()

        # --- What is really on that disk, according to somebody else
        for path, want in (("/EFI/ThreadOS/BOOTX64.EFI", want_loader),
                           ("/EFI/ThreadOS/threados64.bin", want_kernel),
                           ("/EFI/ThreadOS/initrd64.img", want_initrd),
                           ("/EFI/BOOT/BOOTX64.EFI", want_loader)):
            got = fat_get(target, ESP_START, path)
            check(f"{path} byte for byte ({len(want)} B)",
                  got is not None and got == want)

        got = fat_get(target, ESP_START, "/EFI/Microsoft/Boot/bootmgfw.efi")
        check("somebody elses bootmgfw.efi is untouched",
              got == FOREIGN_BOOTMGR)
        check("the GPT is untouched", gpt_intact(target))
        check("the neighbouring Windows partition is untouched",
              neighbours_intact(target, which=(1,)))

        # --- The proof: the disk boots on its own, with no stick
        shutil.copy(OVMF_VARS, nvram)
        q2 = Qemu(KERNEL, INITRD, disk=target, sata=True, serial=slog2,
                  qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            out = q2.wait_serial("threadboot: ThreadOS", timeout=60)
            check("the installed disk boots on its own, with no stick",
                  "threadboot" in out)
            out = q2.wait_serial("handing the machine", timeout=30)
            check(f"the loader finds the kernel where the installer put it "
                  f"({len(want_kernel)} B)",
                  f"kernel: {len(want_kernel)} B" in out)
            out = q2.wait_serial("The 64-bit system is ready", timeout=90)
            check("and the system on that disk reaches the shell",
                  "Multiboot OK (Multiboot 2)" in out)
        finally:
            q2.close()

        # --- And the other case: a machine that already has a loader
        # under the name the firmware falls back to
        check("a second disk - with somebody elses /EFI/BOOT/BOOTX64.EFI - is ready",
              make_target(taken, with_fallback=True))
        shutil.copy(OVMF_VARS, nvram)
        q3 = Qemu(KERNEL, INITRD, esp=esp, disk=taken, sata=True,
                  serial=slog3, qemu="qemu-system-x86_64",
                  uefi=(OVMF_CODE, nvram))
        try:
            q3.wait_serial("The 64-bit system is ready", timeout=120)
            out = q3.command("install text", "Type YES", timeout=30)
            check("the installer sees that /EFI/BOOT/BOOTX64.EFI is taken",
                  "IS TAKEN ALREADY" in out)
            check("and it says up front that a firmware entry will be needed",
                  "boot menu" in out)
            out = q3.command("YES", "Done", timeout=90)
            check("it writes its files all the same",
                  "/EFI/ThreadOS/threados64.bin - done" in out)
            check("but it does not talk about writing over somebody else's file",
                  "/EFI/BOOT/BOOTX64.EFI - done" not in out
                  and "add an entry in the firmware" in out)
        finally:
            q3.close()

        got = fat_get(taken, ESP_START, "/EFI/BOOT/BOOTX64.EFI")
        check("the foreign loader under /EFI/BOOT stayed, byte for byte",
              got == FOREIGN_FALLBACK)
        got = fat_get(taken, ESP_START, "/EFI/ThreadOS/threados64.bin")
        check("and our kernel is in place anyway",
              got is not None and got == want_kernel)
        listing = fat_dir(taken, ESP_START)
        check("mtools sees both directories side by side",
              "ThreadOS" in listing and "Microsoft" in listing)
    finally:
        shutil.rmtree(esp, ignore_errors=True)
        for f in (target, taken, slog, slog2, slog3, nvram):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
