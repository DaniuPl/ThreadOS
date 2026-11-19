#!/usr/bin/env python3
"""ThreadOS smoke test: filesystems that do not trust their own disks.

A filesystem's block numbers and cluster chains are the disk's word,
and a disk can be damaged, or made by somebody else, or be a stick that
was handed over. A review found that the kernel believed them:

  TFS3 read any block an inode named. TFS3 covers 32 MiB of its disk
  (its bitmap's reach); here the disk is 40 MiB, the system formats it
  with TFS3 ('format3'), writes a file, the host writes a marker past
  TFS3's end and points the file's first block at it. TFS3 is now read
  once, when the next start moves it to TFS4 (kernel/tfs3to4.c), from a
  copy of its 32 MiB: the move must not carry the marker over - reading
  past the filesystem is reading somebody else's partition - and must
  say which file it could not move whole; the system still answers.

  TFS4 is the ext2 format: Linux's mke2fs makes one of 32 MiB on a disk
  of 40, and the host damages it the four ways docs/tfs4-plan.md names -
  a file's block pointer past the file system's end (a marker behind
  it), two directories each the other's parent, a directory entry with
  a record length of 0 (a reader that trusts it never moves on), and an
  entry naming an inode past the file system's last. The marker stays
  unseen, the loop and the zero length end, the bad entry is refused,
  and the system still answers.

  FAT walked a directory's chain until an end-of-directory entry. Here
  mtools makes a FAT32 stick with a directory, and the host makes the
  directory's cluster point at itself and fills it with deleted entries,
  so no end ever comes. Listing it must end (a directory holds at most
  65536 entries), and the system still answers; without that the kernel
  went round for ever.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, make_mbr_fat_disk, fat_mkdirs, TfsImage,
                   TFS_BLOCK, TFS_BITMAP, TFS_BITMAP_BLOCKS, TFS_NAME,
                   e2fsprogs_missing)

DISK_MIB = 40
MARKER = b"OUTSIDE-THE-FILESYSTEM;" * 200
PROMPT = "thread64>"


def sudo_format(q, command):
    q.command("sudo " + command, "password:", timeout=20)
    q.command("thread", "Type YES", timeout=20)
    return q.command("YES", PROMPT, timeout=120)


def tfs3_outside():
    disk = tempfile.mktemp(suffix=".img")
    log = tempfile.mktemp(suffix=".log")
    try:
        with open(disk, "wb") as fh:
            fh.truncate(DISK_MIB * 1024 * 1024)
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=log,
                 sata=True)
        try:
            q.wait_serial(PROMPT, timeout=90)
            out = sudo_format(q, "format3")
            check("the disk is TFS3 ('format3')",
                  "is a ThreadOS TFS3 now" in out)
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
            boot = q.wait_serial(PROMPT, timeout=90)
            check("the next start moves TFS3 to TFS4",
                  "moved over from TFS3" in boot)
            check("and says which file it could not move whole ('%s')"
                  % next((l.strip() for l in boot.splitlines()
                          if "could not be moved" in l), "no line"),
                  "/victim.txt could not be moved whole" in boot)
            out = q.command("cat victim.txt", "thread64", timeout=30)
            check("a block number past TFS3 is not read - the marker "
                  "behind it is not carried over",
                  "OUTSIDE-THE-FILESYSTEM" not in out)
            out = q.command("whoami", "thread64", timeout=30)
            check("and the system still answers", "guest" in out)
        finally:
            q.close()
    finally:
        for f in (disk, log):
            if os.path.exists(f):
                os.unlink(f)


def debugfs(img, *requests):
    out = ""
    for r in requests:
        out += subprocess.run(["debugfs", "-w", "-R", r, img],
                              capture_output=True, text=True).stdout
    return out


def inode_of(img, path):
    out = subprocess.run(["debugfs", "-R", "stat " + path, img],
                         capture_output=True, text=True).stdout
    m = re.search(r"Inode:\s+(\d+)", out)
    return int(m.group(1)) if m else None


def first_block(img, path):
    out = subprocess.run(["debugfs", "-R", "blocks " + path, img],
                         capture_output=True, text=True).stdout.split()
    return int(out[0]) if out else None


def entry_at(block, name):
    """Where in a directory block the entry of this name starts"""
    k = 0
    while k + 8 <= len(block):
        rec, nl = struct.unpack_from("<HB", block, k + 4)
        if block[k + 8:k + 8 + nl] == name:
            return k
        if rec < 8:
            break
        k += rec
    return None


def tfs4_damaged():
    work = tempfile.mkdtemp()
    disk = os.path.join(work, "disk.img")
    log = os.path.join(work, "s.log")
    bs = 4096
    try:
        tree = os.path.join(work, "tree")
        for d in ("loop/a/b", "zero", "bad"):
            os.makedirs(os.path.join(tree, d))
        for path, text in [("victim.txt", "written-inside-the-filesystem\n"),
                           ("zero/one.txt", "one\n"),
                           ("zero/two.txt", "two\n"),
                           ("bad/real.txt", "real\n")]:
            with open(os.path.join(tree, path), "w") as fh:
                fh.write(text)
        with open(disk, "wb") as fh:
            fh.truncate(DISK_MIB * 1024 * 1024)
        made = subprocess.run(["mke2fs", "-q", "-F", "-t", "ext2", "-b",
                               str(bs), "-L", "ThreadOS", "-d", tree, disk,
                               "32M"], capture_output=True)
        check("Linux's mke2fs makes a 32 MiB TFS4 on a 40 MiB disk",
              made.returncode == 0)
        if made.returncode:
            return

        # a block pointer past the end, a marker behind it
        outside = (32 << 20) // bs + 1000
        with open(disk, "r+b") as fh:
            fh.seek(outside * bs)
            fh.write(MARKER[:bs])
        debugfs(disk, "sif /victim.txt block[0] %d" % outside)
        # two directories, each the other's parent: b holds a name for a,
        # and a's ".." is b
        debugfs(disk, "ln /loop/a /loop/a/b/back")
        a, b = inode_of(disk, "/loop/a"), inode_of(disk, "/loop/a/b")
        blk_a, blk_zero = first_block(disk, "/loop/a"), \
            first_block(disk, "/zero")
        # an entry naming an inode past the last
        debugfs(disk, "ln /bad/real.txt /bad/ghost")
        blk_bad = first_block(disk, "/bad")
        ok = None not in (a, b, blk_a, blk_zero, blk_bad)
        with open(disk, "r+b") as fh:
            def patch(blk, name, at, fmt, value):
                fh.seek(blk * bs)
                block = fh.read(bs)
                k = entry_at(block, name)
                if k is None:
                    return False
                fh.seek(blk * bs + k + at)
                fh.write(struct.pack(fmt, value))
                return True
            ok = ok and patch(blk_a, b"..", 0, "<I", b)
            ok = ok and patch(blk_zero, b"..", 4, "<H", 0)
            ok = ok and patch(blk_bad, b"ghost", 0, "<I", 999999)
        check("the host damages it four ways", bool(ok))
        if not ok:
            return

        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=log,
                 sata=True)
        try:
            boot = q.wait_serial(PROMPT, timeout=90)
            check("the damaged TFS4 is mounted", "TFS4: " in boot and
                  "+ TFS ready" in boot)
            out = q.command("cat victim.txt", "thread64", timeout=30)
            check("a block number past the file system is not read - the "
                  "marker behind it stays unseen",
                  "OUTSIDE-THE-FILESYSTEM" not in out)
            out = q.command("cd /loop/a", "thread64", timeout=30)
            check("a directory in a loop of parents is entered",
                  "thread64" in out and "cd:" not in out)
            out = q.command("pwd", "thread64", timeout=60)
            lines = [l.strip() for l in out.replace("\r", "").split("\n")]
            answer = next((l for l in lines[1:] if l and
                           not l.startswith("thread64")), "")
            check("its path is asked for, and an answer comes, and the "
                  "shell after it ('%s')" % answer,
                  "thread64" in out and answer.startswith(("/", "pwd")))
            out = q.command("cd /zero", "thread64", timeout=30)
            check("a directory with a record of length 0 is entered",
                  "thread64" in out and "cd:" not in out)
            out = q.command("fls", "Used:", timeout=60)
            check("and listed, and the listing ends", "Used:" in out)
            q.command("cd /bad", "thread64", timeout=30)
            out = q.command("fls", "Used:", timeout=60)
            check("a directory naming an inode past the last is listed: "
                  "the real file, not the bad one",
                  "real.txt" in out and "ghost" not in out)
            out = q.command("cat ghost", "thread64", timeout=30)
            check("and the bad name does not open",
                  "there is no file 'ghost'" in out)
            out = q.command("whoami", "thread64", timeout=30)
            check("and the system still answers", "guest" in out)
        finally:
            q.close()
    finally:
        shutil.rmtree(work, ignore_errors=True)


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
    missing = e2fsprogs_missing()
    if missing:
        check("%s (e2fsprogs) is installed - the test needs it" % missing,
              False)
        return summary()
    tfs3_outside()
    tfs4_damaged()
    fat_loop()
    return summary()


if __name__ == "__main__":
    sys.exit(main())
