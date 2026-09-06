#!/usr/bin/env python3
"""ProtonOS smoke test: several disks, and choosing between them.

The block layer used to find one disk and that was the disk. It is a
list now, and the reason is the installer: on a machine with more than
one disk, "which one" is the whole question, and a choice that cannot
be made is not a choice.

Two things matter here and both are easy to get wrong. That the list
is the machine's real list - every AHCI port with something on it, and
the old bus behind them, in an order that leaves a machine with ONE
disk behaving exactly as it did. And that LOOKING at another disk
changes nothing: reading its table means pointing the machine at it,
and a machine left pointed somewhere it was never mounted from is a
filesystem reading a stranger's sectors.

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
from ptest import (check, summary, no64, make_gpt_disk, mtools_ok, fat_format,
                   fat_mkdirs, fat_put, fat_get, fat_kind, gpt_read,
                   GUID_ESP4, GUID_PFS4)

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
LOADER = os.path.join(ROOT, "build", "BOOTX64.EFI")


def blob(path):
    with open(path, "rb") as fh:
        return fh.read()


def main():
    print()
    print("--- several disks and choosing between them ---")

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
    first = tempfile.mktemp(suffix=".img")
    second = tempfile.mktemp(suffix=".img")
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")
    nvram = tempfile.mktemp(suffix=".fd")

    want_kernel = blob(KERNEL64)

    try:
        os.makedirs(os.path.join(esp, "EFI", "BOOT"))
        shutil.copy(LOADER, os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI"))
        shutil.copy(KERNEL64, os.path.join(esp, "protonos64.bin"))
        shutil.copy(INITRD64, os.path.join(esp, "initrd64.img"))

        # The first disk belongs to somebody: a table, an EFI partition,
        # their loader on it. The second is 400 MiB of nothing.
        make_gpt_disk(first)
        fat_format(first, 2048, 204800, fat32=True, label="LAPTOPESP")
        fat_mkdirs(first, 2048, ["/EFI", "/EFI/Microsoft"])
        fat_put(first, 2048, b"MZ-somebody-elses" * 400,
                "/EFI/Microsoft/bootmgfw.efi")
        with open(second, "wb") as fh:
            fh.truncate(400 * 1024 * 1024)

        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, esp=esp, disk=first, disk2=second,
                 sata=True, serial=slog, qemu="qemu-system-x86_64",
                 uefi=(OVMF_CODE, nvram))
        try:
            q.wait_serial("The 64-bit system is ready", timeout=120)

            out = q.command("install text", "Type YES", timeout=40)
            check("the system lists the disks it has",
                  "disk 1: AHCI" in out and "disk 2: AHCI" in out)
            check("and shows which one it is looking at", "800 MiB  <-" in out)
            check("the first is the one it uses - as before",
                  "Disk 1 of" in out and "LAPTOPESP" in out)
            q.command("no", "install:", timeout=20)

            # The second disk: no table, so there is nothing to install
            # beside - and the system says which disk it is talking
            # about rather than which disk it happens to be using
            out = q.command("install text 2", "install:", timeout=40)
            check("the second disk can be asked about",
                  "400 MiB  <-" in out)
            check("and it tells the truth about the second: it has no table",
                  "no partition table" in out)

            # And nothing moved: asked again without a number, the
            # system talks about the disk it is USING, which is still
            # the first one. Looking at another disk means pointing
            # the machine at it, and a machine left pointed somewhere
            # it was never mounted from is a filesystem reading a
            # stranger's sectors.
            out = q.command("install text", "Type YES", timeout=40)
            check("after looking at the second disk the system still uses "
                  "the first",
                  "Disk 1 of" in out and "800 MiB  <-" in out)
            q.command("no", "install:", timeout=20)

            # The 'disk' command writes a pattern into a sector and
            # reads it back, and the sector it used was the last one -
            # which was safe while our disks had nothing on them. On a
            # GPT disk the last sector IS the copy of the partition
            # table, and a write there destroys it silently: the disk
            # still boots, and one day a repair tool says the table is
            # damaged. This test found that; the assertion is what
            # keeps it found.
            out = q.command("disk", "proton64>", timeout=20)
            check("'disk' on a disk with a table only reads it",
                  "I only read it" in out)
            ok, after, why2 = gpt_read(first)
            check("and the table's copy at the end of the disk is alive%s"
                  % ("" if ok else ": " + why2), ok and len(after) == 3)

            # Now install onto the second, wiping it - and the first
            # one must come out of it untouched
            q.command("install text whole 2", "Type ERASE", timeout=40)
            out = q.command("ERASE", "Done", timeout=240)
            check("installing onto the named disk runs to the end",
                  "partition table (GPT) - done" in out
                  and "/EFI/BOOT/BOOTX64.EFI - done" in out)
        finally:
            q.close()

        # --- what is on each of them, read here
        ok, parts, why = gpt_read(second)
        check("the second disk has our table now%s"
              % ("" if ok else ": " + why), ok and len(parts) == 2)
        if len(parts) == 2:
            check("with an EFI partition and ours", parts[0][0] == GUID_ESP4
                  and parts[1][0] == GUID_PFS4)
        check("its EFI partition holds a FAT32",
              fat_kind(second, 2048) == "FAT32")
        got = fat_get(second, 2048, "/EFI/ProtonOS/protonos64.bin")
        check("and the kernel byte for byte (%d B)" % len(want_kernel),
              got is not None and got == want_kernel)

        ok, still, why2 = gpt_read(first)
        check("the FIRST disk is untouched - the same table (%s)"
              % (why2 or ", ".join(e[3] for e in still)),
              ok and len(still) == 3 and still[1][3] == "Windows")
        check("and somebody else's loader is still on it",
              (fat_get(first, 2048, "/EFI/Microsoft/bootmgfw.efi") or b"")
              .startswith(b"MZ-somebody-elses"))
        check("and our files did NOT land on it",
              fat_get(first, 2048, "/EFI/ProtonOS/protonos64.bin") is None)

        # --- and it boots, from the disk that was chosen
        shutil.copy(OVMF_VARS, nvram)
        q2 = Qemu(KERNEL, INITRD, disk=second, sata=True, serial=slog2,
                  qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            out = q2.wait_serial("protoboot: ProtonOS", timeout=60)
            check("the named disk boots on its own", "protoboot" in out)
            out = q2.wait_serial("The 64-bit system is ready", timeout=120)
            check("and the system on it reaches the shell",
                  "Multiboot OK (Multiboot 2)" in out)
        finally:
            q2.close()
    finally:
        shutil.rmtree(esp, ignore_errors=True)
        for f in (first, second, slog, slog2, nvram):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
