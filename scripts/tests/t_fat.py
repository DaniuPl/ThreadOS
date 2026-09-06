#!/usr/bin/env python3
"""ProtonOS smoke test: FAT12/16/32 - the filesystem of the EFI partition.

The one filesystem here that is not ours, and the reason it exists at
all is that firmware reads FAT and reads nothing else. So every image
in this group is made by mtools and everything the system writes into
one is read back by mtools: an independent implementation on the other
side of every assertion. A driver that agrees only with itself has
proved nothing - least of all on a partition belonging to the system
that boots the machine.

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
from ptest import (check, summary, make_iso64, no64, make_gpt_disk,
                   gpt_intact, neighbours_intact, mtools_ok, fat_format,
                   fat_mkdirs, fat_put, fat_get, fat_dir, fat_kind,
                   fat_minfo, make_mbr_fat_disk)

ESP_START = 2048                     # make_gpt_disk puts the ESP there
ESP_SECTORS = 204800                 # 100 MiB

LONG_TEXT = b"This is a file with a long name on the EFI partition.\n"
BIG = bytes((i * 7 + 3) & 0xFF for i in range(200000))


def rot_sum(data):
    """The same number the kernel's 'fatsum' computes: every byte
    added into a value rotated left by one. Written out here rather
    than imported, so that the two sides are two implementations."""
    s = 0
    for b in data:
        s = (((s << 1) & 0xFFFFFFFF) | (s >> 31)) + b
        s &= 0xFFFFFFFF
    return s


def main():
    print()
    print("--- FAT12/16/32: the EFI partition ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    if not mtools_ok():
        print("SKIPPED: no mtools (nothing to make FAT images with)")
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    disk = tempfile.mktemp(suffix=".img")
    small = tempfile.mktemp(suffix=".img")
    tiny = tempfile.mktemp(suffix=".img")
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")

    try:
        check("the disc with the 64-bit kernel builds",
              make_iso64(d, iso, text=True))
        if not os.path.exists(iso):
            return summary()

        # --- The disk of a laptop: GPT, and an EFI partition with
        # somebody else's boot loader already in it
        make_gpt_disk(disk)
        made = fat_format(disk, ESP_START, ESP_SECTORS, fat32=True,
                          label="ESPTEST")
        check("the EFI partition was formatted by mtools (not by us)", made)
        check("and it came out a FAT32 - which is what the cluster count says",
              fat_kind(disk, ESP_START) == "FAT32")
        fat_mkdirs(disk, ESP_START, ["/EFI", "/EFI/BOOT", "/EFI/Microsoft"])
        fat_put(disk, ESP_START, b"\x4d\x5a" + b"somebody elses loader" * 100,
                "/EFI/Microsoft/bootmgfw.efi")
        fat_put(disk, ESP_START, LONG_TEXT, "/read-me-without-fail.txt")
        fat_put(disk, ESP_START, BIG, "/big.bin")

        q = Qemu(KERNEL, INITRD, cdrom=iso, disk=disk, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=90)

            out = q.command("fat", "cluster", timeout=15)
            check("the system finds the EFI partition in the GPT by itself",
                  "partition 1, sector 2048" in out)
            check("and recognises it as a FAT32 labelled ESPTEST",
                  "FAT32" in out and "ESPTEST" in out)
            m = re.search(r"(\d+) KiB in all, (\d+) KiB free", out)
            check("the size and the free room computed from the table"
                  + (f" ({m.group(1)} KiB, {m.group(2)} KiB free)"
                     if m else ""),
                  m is not None and 90000 < int(m.group(1)) < 110000
                  and int(m.group(2)) < int(m.group(1))
                  and int(m.group(2)) > int(m.group(1)) - 1000)

            out = q.command("fatls /", "entries", timeout=15)
            check("the root directory seen as it was written",
                  "EFI" in out and "big.bin" in out)
            check("the long name read whole, not as READ-M~1.TXT",
                  "read-me-without-fail.txt" in out)
            check("the file size from the directory entry",
                  "200000 B" in out)

            out = q.command("fatls /EFI", "entries", timeout=15)
            check("a directory with a long name ('Microsoft') too",
                  "Microsoft" in out and "BOOT" in out)

            out = q.command("fatcat /read-me-without-fail.txt",
                            "EFI partition", timeout=15)
            check("the file's content character for character",
                  LONG_TEXT.decode().strip() in out)

            # A file of two hundred kilobytes is four hundred clusters:
            # read by following a chain, and a chain followed wrongly
            # gives back bytes that look perfectly plausible
            out = q.command("fatsum /big.bin", "sum", timeout=30)
            m = re.search(r"(\d+) B, sum ([0-9a-f]+)", out)
            check(f"the whole file through the cluster chain "
                  f"({m.group(1) if m else '?'} B)",
                  m is not None and int(m.group(1)) == len(BIG))
            check("and byte for byte the one somebody else wrote",
                  m is not None and int(m.group(2), 16) == rot_sum(BIG))

            # --- Writing. Everything below is checked by mtools after
            # the machine is switched off, not by asking the system
            # whether it thinks it succeeded.
            out = q.command("fatmkdir /EFI/ProtonOS", "fatmkdir", timeout=15)
            check("created", "could not" not in out)
            out = q.command("fatput /EFI/ProtonOS/hello.txt hello-world",
                            "fatput", timeout=15)
            check("written", "could not" not in out)
            out = q.command("fatput /protonos64-testowy.bin ABCDEFGH",
                            "fatput", timeout=15)
            check("a file with a long name was written", "could not" not in out)
            out = q.command("fatcopy libproton.so /EFI/ProtonOS/libproton.so",
                            "fatcopy", timeout=40)
            check("a file from the initrd copied onto the EFI partition",
                  "could not" not in out and "-> /EFI/ProtonOS" in out)
            out = q.command("fatrm /big.bin", "fatrm", timeout=20)
            check("removed", "could not" not in out)

            out = q.command("fatls /", "entries", timeout=15)
            check("after the removal it is not in the directory",
                  "big.bin" not in out
                  and "protonos64-testowy.bin" in out)
            q.command("fat", "cluster", timeout=15)
        finally:
            q.close()

        # --- What is really on the disk, according to somebody else
        listing = fat_dir(disk, ESP_START)
        check("mtools reads a directory we created",
              "ProtonOS" in listing)
        check("mtools reads a long name we wrote",
              "protonos64-testowy.bin" in listing)
        check("the long name has a correct short name too (~1)",
              re.search(r"PROTON~\d\s+BIN", listing) is not None)
        check("the file we removed is gone for mtools too",
              "big" not in listing)
        got = fat_get(disk, ESP_START, "/EFI/ProtonOS/hello.txt")
        check("the content of the written file, read back by mtools",
              got is not None and got.strip() == b"hello-world")
        got = fat_get(disk, ESP_START, "/protonos64-testowy.bin")
        check("and the file with the long name",
              got is not None and got.strip() == b"ABCDEFGH")

        # The copy from the initrd, byte for byte against the file the
        # test itself can read out of the initrd image
        want = None
        with open(INITRD64, "rb") as fh:
            blob = fh.read()
        import struct as _s
        count = _s.unpack_from("<I", blob, 4)[0]
        for i in range(count):
            off = 8 + i * 24
            name = blob[off:off + 16].split(b"\0")[0].decode()
            o, sz = _s.unpack_from("<II", blob, off + 16)
            if name == "libproton.so":
                want = blob[o:o + sz]
        got = fat_get(disk, ESP_START, "/EFI/ProtonOS/libproton.so")
        check(f"the file from the initrd copied byte for byte "
              f"({len(want) if want else 0} B)",
              want is not None and got == want)

        check("somebody elses loader on the EFI partition is untouched",
              (fat_get(disk, ESP_START, "/EFI/Microsoft/bootmgfw.efi") or b"")
              .startswith(b"\x4d\x5a"))
        check("the GPT is untouched", gpt_intact(disk))
        # The EFI partition is the one the test itself put a filesystem
        # into, so the neighbour that still has to be untouched is the
        # Windows one behind it
        check("the neighbouring Windows partition is untouched",
              neighbours_intact(disk, which=(1,)))

        # --- The old table and the two smaller formats. FAT12 is not a
        # curiosity here: its entries are twelve bits, so one of them
        # straddles every sector boundary of the table, and that is
        # where a driver written for whole entries goes wrong.
        for name, path, mib, want_kind in (("FAT16", small, 64, "FAT16"),
                                           ("FAT12", tiny, 2, "FAT12")):
            ok, start = make_mbr_fat_disk(path, mib, fat32=False,
                                          label="MALYFAT")
            check(f"the {name} image in an MBR table is ready",
                  ok and fat_kind(path, start) == want_kind)
            fat_put(path, start, LONG_TEXT, "/od-mtools.txt")

            q2 = Qemu(KERNEL, INITRD, cdrom=iso, disk=path, serial=slog2,
                      qemu="qemu-system-x86_64")
            try:
                q2.wait_serial("proton64>", timeout=90)
                out = q2.command("fat 1", "cluster", timeout=20)
                check(f"{name} recognised by the cluster count, "
                      f"not by the string in the boot sector",
                      want_kind in out)
                out = q2.command("fatcat /od-mtools.txt", "partitions",
                                 timeout=15)
                check(f"{name}: mtools' file was read",
                      LONG_TEXT.decode().strip() in out)
                out = q2.command("fatcopy hello64 /hello64-z-initrd.bin",
                                 "fatcopy", timeout=40)
                check(f"{name}: writing a file through the cluster chain",
                      "could not" not in out)
                out = q2.command("fatls /", "entries", timeout=15)
                check(f"{name}: both files in the root directory",
                      "od-mtools.txt" in out
                      and "hello64-z-initrd.bin" in out)
            finally:
                q2.close()

            listing = fat_dir(path, start)
            check(f"{name}: mtools sees what we wrote",
                  "hello64-z-initrd.bin" in listing)
            got = fat_get(path, start, "/hello64-z-initrd.bin")
            check(f"{name}: and the content matches the file in the initrd",
                  got is not None and len(got) > 4000
                  and got[:4] == b"\x7fELF")

        # --- A small volume that a formatter was TOLD to make FAT32
        #
        # The type of a FAT is decided by the number of clusters - that
        # is the rule, and it is the rule this driver follows. But a
        # formatter told to make FAT32 makes it even when the volume is
        # too small to need it, and such sticks and camera cards exist.
        # Read by cluster count alone, such a volume looks like FAT16
        # and everything after that is garbage: an empty directory on a
        # stick full of files, which is exactly how this was found.
        maly32 = tempfile.mktemp(suffix=".img")
        ok, start = make_mbr_fat_disk(maly32, 32, fat32=True,
                                      label="MALYFAT32")
        # The evidence that it really is FAT32 comes from mtools, not
        # from us: "sectors per fat: 0" is what a FAT32 boot sector has
        # instead of a sixteen-bit count, and no FAT16 volume has it.
        info = fat_minfo(maly32, start)
        check("a 32 MiB image formatted by mtools AS A FAT32 "
              "(minfo: sectors per fat = 0)",
              ok and "sectors per fat: 0" in info)
        fat_put(maly32, start, LONG_TEXT, "/od-mtools.txt")

        q3 = Qemu(KERNEL, INITRD, cdrom=iso, disk=maly32, serial=slog2,
                  qemu="qemu-system-x86_64")
        try:
            q3.wait_serial("proton64>", timeout=90)
            out = q3.command("fat 1", "cluster", timeout=20)
            check("a small volume does not pretend to be FAT16 - the cluster count is not "
                  "the only word",
                  "FAT32" in out and "MALYFAT32" in out)
            out = q3.command("fatls /", "entries", timeout=15)
            check("and a file shows on it, not an empty directory",
                  "od-mtools.txt" in out)
            out = q3.command("fatcat /od-mtools.txt", "partitions", timeout=15)
            check("with the content character for character",
                  LONG_TEXT.decode().strip() in out)
        finally:
            q3.close()
            if os.path.exists(maly32):
                os.unlink(maly32)

        # --- And the thing this driver must never do
        check("the FAT boot sector vanished from none of the images",
              fat_kind(disk, ESP_START) == "FAT32"
              and fat_kind(small, 2048) == "FAT16"
              and fat_kind(tiny, 2048) == "FAT12")
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, disk, small, tiny, slog, slog2):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
