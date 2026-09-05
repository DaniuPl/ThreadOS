#!/usr/bin/env python3
"""Makes a bootable ProtonOS stick image.

What a UEFI machine boots from is not a disc and not a kernel: it is a
disk with a GPT on it, an EFI system partition inside, a FAT
filesystem inside that, and a loader at a name the firmware knows. So
that is what this writes - a raw image to put on a stick with

    dd if=build/protonos-stick.img of=/dev/sdX bs=4M conv=fsync

and nothing else. No GRUB, no El Torito, no hybrid anything: the three
files ProtonOS starts from, on a partition the firmware will look in.

The table is written here, byte by byte, for the same reason the
kernel's own writer exists: a firmware checks the CRCs and the copy at
the end of the disk, and quietly decides a table is damaged. The FAT
is made by mformat, which is somebody else's implementation - and
having the stick made by one and read by another is worth more than
having both sides be ours.
"""

import argparse
import binascii
import os
import struct
import subprocess
import sys

SECTOR = 512
ENTRIES = 128
ENT_SIZE = 128
ENT_SECT = (ENTRIES * ENT_SIZE) // SECTOR          # 32

# C12A7328-F81F-11D2-BA4B-00A0C93EC93B, in the order the format wants
GUID_ESP = bytes([0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                  0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B])


def uuid_from(seed, n):
    """A GUID that is unique among the ones on this image. Not random -
    there is nothing to be random with here - and honest about it."""
    out = bytearray(16)
    for i in range(16):
        out[i] = (seed >> ((i & 3) * 8)) & 0xFF
    out[0] = n & 0xFF
    out[15] = 0x80 | (n & 0x3F)
    return bytes(out)


def utf16_name(name):
    raw = name.encode("utf-16-le")[:70]
    return raw + b"\0" * (72 - len(raw))


def write_gpt(path, total, first, last):
    """A protective MBR, the header and the entries at the front, and
    the same entries and a header pointing the other way at the very
    end. The last thirty-four sectors of a GPT disk are that copy."""
    ent = bytearray(ENTRIES * ENT_SIZE)
    e = bytearray(ENT_SIZE)
    e[0:16] = GUID_ESP
    e[16:32] = uuid_from(total, 1)
    e[32:40] = struct.pack("<Q", first)
    e[40:48] = struct.pack("<Q", last)
    e[56:128] = utf16_name("EFI")
    ent[0:ENT_SIZE] = e
    ent_crc = binascii.crc32(bytes(ent)) & 0xFFFFFFFF
    disk_id = uuid_from(total, 0)

    def header(self_lba, other_lba, ent_lba):
        h = bytearray(92)
        h[0:8] = b"EFI PART"
        h[8:12] = struct.pack("<I", 0x00010000)
        h[12:16] = struct.pack("<I", 92)
        h[24:32] = struct.pack("<Q", self_lba)
        h[32:40] = struct.pack("<Q", other_lba)
        h[40:48] = struct.pack("<Q", first)
        h[48:56] = struct.pack("<Q", last)
        h[56:72] = disk_id
        h[72:80] = struct.pack("<Q", ent_lba)
        h[80:84] = struct.pack("<I", ENTRIES)
        h[84:88] = struct.pack("<I", ENT_SIZE)
        h[88:92] = struct.pack("<I", ent_crc)
        h[16:20] = struct.pack("<I", binascii.crc32(bytes(h)) & 0xFFFFFFFF)
        return bytes(h)

    mbr = bytearray(SECTOR)
    mbr[446 + 1] = 0x00
    mbr[446 + 2] = 0x02
    mbr[446 + 4] = 0xEE
    mbr[446 + 5] = 0xFF
    mbr[446 + 6] = 0xFF
    mbr[446 + 7] = 0xFF
    mbr[446 + 8:446 + 12] = struct.pack("<I", 1)
    mbr[446 + 12:446 + 16] = struct.pack("<I", min(total - 1, 0xFFFFFFFF))
    mbr[510] = 0x55
    mbr[511] = 0xAA

    with open(path, "wb") as fh:
        fh.truncate(total * SECTOR)
    with open(path, "r+b") as fh:
        fh.write(bytes(mbr))
        fh.write(header(1, total - 1, 2))
        fh.seek(2 * SECTOR)
        fh.write(bytes(ent))
        fh.seek((total - 1 - ENT_SECT) * SECTOR)
        fh.write(bytes(ent))
        fh.seek((total - 1) * SECTOR)
        fh.write(header(total - 1, 1, total - 1 - ENT_SECT))


def mtool(args, env):
    r = subprocess.run(args, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    if r.returncode:
        sys.stderr.write(r.stdout.decode(errors="replace"))
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("loader")
    ap.add_argument("kernel")
    ap.add_argument("initrd")
    ap.add_argument("--mib", type=int, default=192)
    args = ap.parse_args()

    for f in (args.loader, args.kernel, args.initrd):
        if not os.path.exists(f):
            sys.stderr.write("mkstick: nie ma %s\n" % f)
            return 1

    total = args.mib * 2048
    first = 2048
    last = total - 34                       # the copy at the end is not ours
    if last - first < 64 * 2048:
        sys.stderr.write("mkstick: %d MiB to za malo\n" % args.mib)
        return 1

    write_gpt(args.out, total, first, last)

    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    at = "%s@@%d" % (args.out, first * SECTOR)
    steps = [
        ["mformat", "-i", at, "-F", "-T", str(last - first + 1),
         "-v", "PROTONOS", "::"],
        ["mmd", "-i", at, "::/EFI"],
        ["mmd", "-i", at, "::/EFI/BOOT"],
        # The name the firmware falls back to when nothing else is
        # arranged, which on a stick is the only name that will be
        # tried at all
        ["mcopy", "-i", at, args.loader, "::/EFI/BOOT/BOOTX64.EFI"],
        # And what the loader itself goes looking for, in the first
        # place it looks
        ["mcopy", "-i", at, args.kernel, "::/protonos64.bin"],
        ["mcopy", "-i", at, args.initrd, "::/initrd64.img"],
    ]
    for step in steps:
        if not mtool(step, env):
            sys.stderr.write("mkstick: nie udalo sie: %s\n" % " ".join(step))
            return 1

    print("Gotowe: %s (%d MiB)" % (args.out, args.mib))
    print("Na pendrive:  dd if=%s of=/dev/sdX bs=4M conv=fsync" % args.out)
    print("UWAGA: /dev/sdX to CALY pendrive, nie partycja - i wszystko,")
    print("       co na nim jest, zniknie.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
