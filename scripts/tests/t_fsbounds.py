#!/usr/bin/env python3
"""ThreadOS smoke test: filesystems that do not trust their own disks.

A filesystem's block numbers and cluster chains are the disk's word,
and a disk can be damaged, or made by somebody else, or be a stick that
was handed over. A review found that the kernel believed them:

  TFS read any block an inode named. The filesystem covers 32 MiB of
  its disk (its bitmap's reach); here the disk is 40 MiB, the host
  writes a marker past the filesystem's end, the system writes a file,
  and the host points the file's first block at the marker. Started
  again, the system must not show the marker - reading past the
  filesystem is reading somebody else's partition - and it still
  answers.

  FAT walked a directory's chain until an end-of-directory entry. Here
  mtools makes a FAT32 stick with a directory, and the host makes the
  directory's cluster point at itself and fills it with deleted entries,
  so no end ever comes. Listing it must end (a directory holds at most
  65536 entries), and the system still answers; without that the kernel
  went round for ever.

Run on its own or through scripts/run_tests.py.
"""

import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, make_mbr_fat_disk, fat_mkdirs, TfsImage,
                   TFS_BLOCK, TFS_BITMAP, TFS_BITMAP_BLOCKS, TFS_NAME)

DISK_MIB = 40
MARKER = b"OUTSIDE-THE-FILESYSTEM;" * 40


def tfs_outside():
    disk = tempfile.mktemp(suffix=".img")
    log = tempfile.mktemp(suffix=".log")
    try:
        with open(disk, "wb") as fh:
            fh.truncate(DISK_MIB * 1024 * 1024)
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=log,
                 sata=True)
        try:
            q.wait_serial("thread64>", timeout=90)
            out = q.command("write victim.txt written-inside-the-filesystem",
                            "thread64", timeout=30)
            out = q.command("cat victim.txt", "inside", timeout=30)
            check("the system writes a file on its own disk",
                  "written-inside-the-filesystem" in out)
        finally:
            q.close()

        img = TfsImage(disk)
        outside = img.nblocks + 1000
        check("the filesystem ends before the disk does (%d of %d blocks)"
              % (img.nblocks, DISK_MIB * 1024),
              outside < DISK_MIB * 1024)
        slot = next((i for i, ino in enumerate(img.inodes)
                     if ino["used"] and ino["name"] == "victim.txt"), None)
        check("the file is in the inode table", slot is not None)
        if slot is None:
            return
        table = TFS_BLOCK * (TFS_BITMAP + TFS_BITMAP_BLOCKS)
        with open(disk, "r+b") as fh:
            fh.seek(outside * TFS_BLOCK)
            fh.write(MARKER[:TFS_BLOCK])
            fh.seek(table + slot * 128 + TFS_NAME + 8)
            fh.write(struct.pack("<I", outside))

        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=log,
                 sata=True)
        try:
            q.wait_serial("thread64>", timeout=90)
            out = q.command("cat victim.txt", "thread64", timeout=30)
            check("a block number past the filesystem is not read - the "
                  "marker behind it stays unseen",
                  "OUTSIDE-THE-FILESYSTEM" not in out)
            out = q.command("whoami", "thread64", timeout=30)
            check("and the system still answers", "guest" in out)
        finally:
            q.close()
    finally:
        for f in (disk, log):
            if os.path.exists(f):
                os.unlink(f)


def fat_loop():
    disk = tempfile.mktemp(suffix=".img")
    log = tempfile.mktemp(suffix=".log")
    try:
        ok, start = make_mbr_fat_disk(disk, mib=64, fat32=True,
                                      label="LOOPFAT")
        ok = ok and fat_mkdirs(disk, start, ["/LOOP"])
        check("mtools makes a FAT32 stick with a directory", ok)
        if not ok:
            return
        with open(disk, "r+b") as fh:
            base = start * 512
            fh.seek(base)
            bs = fh.read(512)
            bps, spc = struct.unpack_from("<HB", bs, 11)
            reserved, nfats = struct.unpack_from("<HB", bs, 14)
            fatsz, = struct.unpack_from("<I", bs, 36)
            root, = struct.unpack_from("<I", bs, 44)
            fat_at = base + reserved * bps
            data_at = fat_at + nfats * fatsz * bps

            def clus_at(c):
                return data_at + (c - 2) * spc * bps
            fh.seek(clus_at(root))
            entries = fh.read(spc * bps)
            loop = None
            for i in range(0, len(entries), 32):
                e = entries[i:i + 32]
                if e[:11] == b"LOOP       ":
                    loop = (struct.unpack_from("<H", e, 20)[0] << 16 |
                            struct.unpack_from("<H", e, 26)[0])
            check("the directory's cluster found (%s)" % loop, loop)
            if not loop:
                return
            for k in range(nfats):         # the chain: itself, for ever
                fh.seek(fat_at + k * fatsz * bps + loop * 4)
                fh.write(struct.pack("<I", loop))
            deleted = (b"\xe5" + b"DELETED    "[1:] + b"\x20" + bytes(20))
            fh.seek(clus_at(loop))
            fh.write(deleted * (spc * bps // 32))

        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=log,
                 sata=True)
        try:
            q.wait_serial("thread64>", timeout=90)
            out = q.command("fat 1", "cluster", timeout=30)
            check("the stick is mounted", "FAT32" in out)
            out = q.command("fatls /LOOP", "entries", timeout=180)
            check("listing a directory whose chain loops back on itself "
                  "ends", "entries" in out)
            out = q.command("whoami", "thread64", timeout=30)
            check("and the system still answers", "guest" in out)
        finally:
            q.close()
    finally:
        for f in (disk, log):
            if os.path.exists(f):
                os.unlink(f)


def main():
    print("--- filesystems that do not trust their disks ---")
    tfs_outside()
    fat_loop()
    return summary()


if __name__ == "__main__":
    sys.exit(main())
