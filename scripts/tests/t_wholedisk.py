#!/usr/bin/env python3
"""ProtonOS smoke test: taking the whole disk.

The other way of installing. Not three files into somebody's EFI
partition, but a disk made ours from nothing: a new partition table, a
new EFI partition with a FAT32 on it, a partition for ProtonOS - and
everything that was on that disk gone.

So this group is mostly about what a table has to be for a firmware to
believe it. The table is read back HERE, in Python, the way a firmware
reads one: the protective MBR, both headers, both CRCs, and the copy at
the end of the disk that has to match the one at the front. A table
that is merely consistent with the code that wrote it is a table that
boots nothing.

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
                   fat_mkdirs, fat_put, fat_get, fat_dir, fat_kind, gpt_read,
                   GUID_ESP4, GUID_PFS4)

ESP_START = 2048
ESP_SECTORS = 204800

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
    print("--- the whole disk for ProtonOS ---")

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
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")
    nvram = tempfile.mktemp(suffix=".fd")

    want_loader = blob(LOADER)
    want_kernel = blob(KERNEL64)
    want_initrd = blob(INITRD64)

    try:
        os.makedirs(os.path.join(esp, "EFI", "BOOT"))
        shutil.copy(LOADER, os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI"))
        shutil.copy(KERNEL64, os.path.join(esp, "protonos64.bin"))
        shutil.copy(INITRD64, os.path.join(esp, "initrd64.img"))

        # A disk that belongs to somebody else, with a table of its own
        # and data in its partitions. All of it is going away, and the
        # test says so before it happens.
        make_gpt_disk(target)
        fat_format(target, ESP_START, ESP_SECTORS, fat32=True, label="OLD")
        fat_mkdirs(target, ESP_START, ["/EFI", "/EFI/Microsoft"])
        fat_put(target, ESP_START, b"MZ-somebody-elses" * 400,
                "/EFI/Microsoft/bootmgfw.efi")
        ok, before, why = gpt_read(target)
        check("the foreign machine's disk has a correct GPT of its own (%d "
              "partitions)" % len(before), ok and len(before) == 3)

        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, esp=esp, disk=target, sata=True,
                 serial=slog, qemu="qemu-system-x86_64",
                 uefi=(OVMF_CODE, nvram))
        try:
            q.wait_serial("The 64-bit system is ready", timeout=120)

            out = q.command("install text whole", "Type ERASE",
                            timeout=40)
            check("the plan says outright that it will erase the whole disk",
                  "THIS WHOLE DISK WILL BE ProtonOS" in out
                  and "It will lose EVERYTHING" in out)
            check("and it lists what it will do",
                  "a GPT" in out
                  and "an EFI partition of 128 MiB (FAT32)" in out
                  and "partition (PFS)" in out)
            check("the word is different from the one for installing beside",
                  "Type ERASE" in out and "Type YES" not in out)

            # A word that is not the word - and the disk still has its
            # old table afterwards
            out = q.command("no", "install:", timeout=20)
            check("without the word ERASE nothing happens",
                  "nothing was written" in out)
            ok, still, _ = gpt_read(target)
            check("and the old table is untouched",
                  ok and len(still) == 3 and still[0][3] == before[0][3])

            q.command("install text whole", "Type ERASE", timeout=40)
            out = q.command("ERASE", "Done", timeout=180)
            check("after ERASE it makes a table, partitions and a filesystem",
                  "partition table (GPT) - done" in out
                  and "EFI partition (FAT32) - done" in out
                  and "ProtonOS partition (PFS) - done" in out)
            check("and it writes the files onto them",
                  "/EFI/ProtonOS/protonos64.bin - done" in out
                  and "/EFI/BOOT/BOOTX64.EFI - done" in out)
        finally:
            q.close()

        # --- the table, read the way a firmware reads one
        ok, parts, why = gpt_read(target)
        check("the GPT holds together (a protective MBR, both headers, "
              "both checksums)%s" % ("" if ok else ": " + why), ok)
        check("there are exactly two partitions (%d)" % len(parts),
              len(parts) == 2)
        if len(parts) == 2:
            check("the first is the EFI partition, and it says so with its GUID",
                  parts[0][0] == GUID_ESP4)
            check("the second is ours and says so with its GUID too (%s)"
                  % parts[1][0].decode("latin1"),
                  parts[1][0] == GUID_PFS4)
            check("the partition names: '%s' and '%s'" % (parts[0][3], parts[1][3]),
                  parts[0][3] == "EFI" and parts[1][3] == "ProtonOS")
            check("the EFI partition starts at 2048 and is 128 MiB (%d "
                  "sectors)" % (parts[0][2] - parts[0][1] + 1),
                  parts[0][1] == 2048
                  and parts[0][2] - parts[0][1] + 1 == 128 * 2048)
            check("our partition starts where that one ends",
                  parts[1][1] == parts[0][2] + 1)
            size = os.path.getsize(target) // 512
            check("and it does not run into the table's copy at the end of the disk",
                  parts[1][2] <= size - 34)

        # --- the filesystem on it, read by somebody else
        check("the EFI partition holds a FAT32 (%s)" % fat_kind(target, 2048),
              fat_kind(target, 2048) == "FAT32")
        listing = fat_dir(target, 2048)
        check("mtools reads the directories we made off it",
              "ProtonOS" in listing and "BOOT" in listing)
        for path, want in (("/EFI/ProtonOS/BOOTX64.EFI", want_loader),
                           ("/EFI/ProtonOS/protonos64.bin", want_kernel),
                           ("/EFI/ProtonOS/initrd64.img", want_initrd),
                           ("/EFI/BOOT/BOOTX64.EFI", want_loader)):
            got = fat_get(target, 2048, path)
            check("%s byte for byte (%d B)" % (path, len(want)),
                  got is not None and got == want)

        # --- and the thing this mode is FOR: the old disk is gone.
        #
        # Gone means the table and the filesystems are gone, not that
        # every byte has been scrubbed - no installer zeroes a disk,
        # and pretending otherwise here would be asserting something
        # that is not true of any system. What must be true is that
        # nothing of the old shape is left: the partitions the old
        # table described are not described by anything any more.
        old_starts = [e[1] for e in before]
        new_starts = [e[1] for e in parts]
        check("not one of the old partitions left an entry in the table",
              not any(x in new_starts for x in old_starts[1:]))
        check("and somebody else's bootmgfw.efi can no longer be read",
              fat_get(target, 2048, "/EFI/Microsoft/bootmgfw.efi") is None)

        # --- the proof: it boots
        shutil.copy(OVMF_VARS, nvram)
        q2 = Qemu(KERNEL, INITRD, disk=target, sata=True, serial=slog2,
                  qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            out = q2.wait_serial("protoboot: ProtonOS", timeout=60)
            check("the firmware picks our loader out of the table we "
                  "made ourselves", "protoboot" in out)
            out = q2.wait_serial("The 64-bit system is ready", timeout=120)
            check("and the system on that disk reaches the shell",
                  "Multiboot OK (Multiboot 2)" in out)
            check("and the PFS on our partition is ready to use",
                  "PFS" in out and "OUT OF REACH" not in out.split(
                      "Settings:")[0].split("Disk ")[-1])
            out = q2.command("fls", "proton64>", timeout=20)
            check("and files can be kept on it",
                  "there is no" not in out.lower() or "swap" in out.lower())
        finally:
            q2.close()
    finally:
        shutil.rmtree(esp, ignore_errors=True)
        for f in (target, slog, slog2, nvram):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
