#!/usr/bin/env python3
"""ThreadOS smoke test: EHCI - USB 2.0, and the controller beside it.

Between UHCI (USB 1.1) and xHCI (USB 3) there were ten years of
machines with EHCI, and on those the system saw either nothing or only
a keyboard: UHCI could not read a stick, and xHCI was not there.

EHCI has a peculiarity the tests have to cover, because it is how real
boards are built: it speaks ONLY to high-speed devices. A keyboard or a
mouse - USB 1.1 devices to this day - is handed over, port by port, to
a "companion" UHCI controller sharing the same sockets. So the shapes
tested here are:

  EHCI alone with a stick on it: the stick is found, its sectors read
  back match what the host put there, and a FAT on it is read and
  written - with mtools on the host checking the result, not the
  system's word for it

  one read of 128 KiB in a single command: more than one transfer
  descriptor holds, so the driver's chain - and the data toggle carried
  across it - is what is being checked. The CRC of what arrived is
  compared with zlib's CRC of the same bytes in the image file

  the board's layout: EHCI with a UHCI companion, a USB 1.1 keyboard
  and a stick side by side. The keyboard must end up on the companion
  and still type; the stick must stay on EHCI and still read

  a USB 2.0 keyboard on EHCI itself - interrupt transfers through the
  periodic schedule, the other half of the driver

  a stick plugged in and pulled out while the machine runs, twice: the
  second time proves what the first one left behind was given back

  and the 64-bit kernel on the same controller, because the driver is
  shared and long mode is where the structures could end up somewhere
  32-bit pointers cannot reach

What is NOT tested, and why: a hub on EHCI. QEMU has no high-speed hub
to put on it, and a low-speed device behind a real one needs split
transactions the driver does not do. The driver refuses a hub out loud
rather than half-working with one.
"""

import os
import re
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, make_mbr_fat_disk, fat_put, fat_get,
                   fat_dir, mtools_ok, make_iso64, no64)

STICK_MIB = 32
FROM_HOST = b"put here by mtools, read over EHCI\n"


def usb_disk_number(listing):
    m = re.search(r"disk (\d+): USB", listing)
    return int(m.group(1)) if m else 0


def crc_matches(q, stick, disk_no, lba, count):
    """One large read by the system, the same bytes read by the host.
    Returns (ok, what the system said)."""
    out = q.command("disks read %d %d %d" % (disk_no, lba, count),
                    "crc32", timeout=30)
    m = re.search(r"crc32 ([0-9a-f]{8})", out)
    with open(stick, "rb") as f:
        f.seek(lba * 512)
        want = zlib.crc32(f.read(count * 512)) & 0xFFFFFFFF
    got = int(m.group(1), 16) if m else None
    return got == want, "%08x vs %08x" % (got or 0, want)


def machine_disk():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    return disk


def stick_with_fat(label="EHCISTICK"):
    stick = tempfile.mktemp(suffix=".usb")
    made, start = make_mbr_fat_disk(stick, mib=STICK_MIB, fat32=True,
                                    label=label)
    if made:
        fat_put(stick, start, FROM_HOST, "/fromhost.txt")
    return stick, made, start


def stick_alone():
    print("--- EHCI alone, a stick on it (the 32-bit kernel) ---")
    stick, made, start = stick_with_fat()
    check("the stick was formatted by mtools (not by us)", made)
    if not made:
        return
    disk = machine_disk()
    slog = tempfile.mktemp(suffix=".log")
    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog,
             devices=["-device", "usb-ehci,id=ehci",
                      "-drive", "if=none,id=st,format=raw,file=" + stick,
                      "-device", "usb-storage,bus=ehci.0,drive=st"])
    try:
        boot = q.wait_serial("thread>", timeout=60)
        m = re.search(r"USB disk: (.*), (\d+) sectors \(port (\d+), (\w+)\)",
                      boot)
        check("the system finds the stick, and says it is on EHCI"
              + (" ('%s')" % m.group(0) if m else ""),
              m is not None and m.group(4) == "EHCI")
        check("its capacity matches the image (%d sectors)"
              % (STICK_MIB * 2048),
              m is not None and int(m.group(2)) == STICK_MIB * 2048)

        out = q.command("hardware", "USB:", timeout=20)
        check("'hardware' names the controller",
              re.search(r"USB:\s+EHCI,", out) is not None)

        out = q.command("disks", "sectors", timeout=20)
        n = usb_disk_number(out)
        check("the stick is on the disk list (disk %d)" % n, n > 0)

        # Sector 0, then a piece that is not a whole page, then 128 KiB
        # in one command - eight transfer descriptors in a chain
        for lba, count, what in ((0, 1, "one sector"),
                                 (7, 33, "33 sectors - two descriptors, "
                                         "the second one not full"),
                                 (2048, 256, "128 KiB in one command - "
                                             "a chain of eight")):
            ok, said = crc_matches(q, stick, n, lba, count)
            check("%s: the CRC matches the host's (%s)" % (what, said), ok)

        out = q.command("fat 1 %d" % n, "cluster", timeout=30)
        check("the FAT on the stick is recognised",
              "FAT32" in out and "EHCISTICK" in out)
        out = q.command("fatcat /fromhost.txt", "EHCI", timeout=20)
        check("the host's file is read character for character",
              FROM_HOST.decode().strip() in out)

        q.command("fatmkdir /fromehci", "fatmkdir", timeout=20)
        out = q.command("fatput /fromehci/note.txt written-over-ehci",
                        "fatput:", timeout=20)
        out = q.command("fatcopy gimg /fromehci/gimg.bin", "gimg.bin, ",
                        timeout=60)
        m = re.search(r"gimg\.bin, (\d+) B", out)
        check("a program from the initrd is copied onto it"
              + (" (%s B)" % m.group(1) if m else ""), m is not None)
        q.command("fat 1 %d" % n, "cluster", timeout=30)   # flush
    finally:
        q.close()

    # --- what the host finds afterwards ------------------------------
    listing = fat_dir(stick, start, "::/fromehci") or ""
    check("mtools sees the directory the system made",
          "NOTE" in listing.upper() and "GIMG" in listing.upper())
    got = fat_get(stick, start, "/fromehci/note.txt")
    check("and the file holds exactly what was written",
          got is not None and b"written-over-ehci" in got)
    want = open(os.path.join(os.path.dirname(INITRD), "user", "gimg.elf"),
                "rb").read()
    copied = fat_get(stick, start, "/fromehci/gimg.bin")
    check("the copied program is byte for byte the one in the build "
          "(%d B)" % len(want), copied == want)
    check("the host's own file is untouched",
          fat_get(stick, start, "/fromhost.txt") == FROM_HOST)
    for f in (stick, disk, slog):
        if os.path.exists(f):
            os.unlink(f)


def board_layout():
    print("--- the board's layout: EHCI, a UHCI companion, a USB 1.1 "
          "keyboard ---")
    stick, made, start = stick_with_fat()
    disk = machine_disk()
    slog = tempfile.mktemp(suffix=".log")
    # The keyboard in port 1 is a USB 1.1 device: EHCI may not keep it.
    # The companion covers ports 1 and 2. No PS/2 at all - every key
    # has to come through the companion or not at all.
    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog,
             no_i8042=True,
             devices=["-device", "ich9-usb-ehci1,id=ehci",
                      "-device", "ich9-usb-uhci1,masterbus=ehci.0,"
                                 "firstport=0",
                      "-device", "usb-kbd,bus=ehci.0,port=1,usb_version=1",
                      "-drive", "if=none,id=st,format=raw,file=" + stick,
                      "-device", "usb-storage,bus=ehci.0,port=3,drive=st"])
    try:
        boot = q.wait_serial("thread>", timeout=60)
        check("EHCI hands the slow keyboard to the companion",
              "socket 1: a slower device - handed to the companion" in boot)
        check("and the keyboard turns up on the companion, as UHCI",
              re.search(r"USB keyboard \(HID, UHCI\)", boot) is not None)
        check("while the stick stays on EHCI",
              re.search(r"USB disk: .*\(port 3, EHCI\)", boot) is not None)

        out = q.command("hardware", "USB:", timeout=20)
        check("'hardware' names both controllers and the keyboard",
              re.search(r"USB:\s+EHCI \+ UHCI, keyboard yes", out)
              is not None)

        out = q.command("uptime", "timer ticks", timeout=20)
        check("a command typed on the companion's keyboard runs",
              "timer ticks" in out)

        out = q.command("disks", "sectors", timeout=20)
        n = usb_disk_number(out)
        ok, said = crc_matches(q, stick, n, 2048, 256)
        check("and the stick beside it reads 128 KiB correctly (%s)" % said,
              ok)
    finally:
        q.close()
    for f in (stick, disk, slog):
        if os.path.exists(f):
            os.unlink(f)


def fast_keyboard():
    print("--- a USB 2.0 keyboard on EHCI itself ---")
    disk = machine_disk()
    slog = tempfile.mktemp(suffix=".log")
    # usb-kbd defaults to USB 2.0, so this one stays on EHCI and is read
    # through the periodic schedule
    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog,
             no_i8042=True,
             devices=["-device", "usb-ehci,id=ehci",
                      "-device", "usb-kbd,bus=ehci.0"])
    try:
        boot = q.wait_serial("thread>", timeout=60)
        check("the keyboard is found on EHCI",
              re.search(r"USB keyboard \(HID, EHCI\)", boot) is not None)
        out = q.command("uptime", "timer ticks", timeout=20)
        check("a command typed on it runs (interrupt transfers, the "
              "periodic schedule)", "timer ticks" in out)
        # Capitals: Shift is a bit in the report, so a lost or repeated
        # report shows up here as the wrong case
        q.command("write caps.txt AbC-dEf", "wrote", timeout=20)
        out = q.command("cat caps.txt", "dEf", timeout=20)
        check("Shift arrives in the right reports ('AbC-dEf')",
              "AbC-dEf" in out)
    finally:
        q.close()
    for f in (disk, slog):
        if os.path.exists(f):
            os.unlink(f)


def hotplug():
    print("--- a stick plugged in and pulled out, twice ---")
    stick, made, start = stick_with_fat(label="LATER")
    disk = machine_disk()
    slog = tempfile.mktemp(suffix=".log")
    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog,
             devices=["-device", "usb-ehci,id=ehci"])
    try:
        q.wait_serial("thread>", timeout=60)
        out = q.command("disks", "sectors", timeout=20)
        check("at the start there is no stick", "USB" not in out)
        q.hmp("drive_add 0 if=none,id=pd,format=raw,file=" + stick)

        for attempt in (1, 2):
            seen = len(q.serial())
            q.cmd({"execute": "device_add",
                   "arguments": {"driver": "usb-storage", "bus": "ehci.0",
                                 "drive": "pd", "id": "pendrive"}})
            for _ in range(60):
                if "USB disk" in q.serial()[seen:]:
                    break
                time.sleep(0.5)
            check("(%d) the stick is noticed while the machine runs"
                  % attempt, "USB disk" in q.serial()[seen:])
            # The number it got, as the kernel says it. Not the first
            # USB line of 'disks': a stick that left keeps its number
            # (somebody may be reading that list), so the second one
            # comes in under a new one.
            m = re.search(r"it is disk (\d+)", q.serial()[seen:])
            n = int(m.group(1)) if m else 0
            ok, said = crc_matches(q, stick, n, 0, 64) if n else (False, "")
            check("(%d) and read correctly (%s)" % (attempt, said), ok)

            seen = len(q.serial())
            q.cmd({"execute": "device_del",
                   "arguments": {"id": "pendrive"}})
            for _ in range(60):
                if "pulled out of socket" in q.serial()[seen:]:
                    break
                time.sleep(0.5)
            check("(%d) and its leaving is noticed too" % attempt,
                  "pulled out of socket" in q.serial()[seen:])
            # QEMU keeps the drive after device_del unless it is told
            # otherwise; give it back for the second round
            time.sleep(1)
            if attempt == 1:
                q.hmp("drive_add 0 if=none,id=pd,format=raw,file=" + stick)

        out = q.command("uptime", "timer ticks", timeout=20)
        check("the machine is still alive afterwards", "timer ticks" in out)
    finally:
        q.close()
    for f in (stick, disk, slog):
        if os.path.exists(f):
            os.unlink(f)


def long_mode():
    print("--- the same controller under the 64-bit kernel ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return
    stick, made, start = stick_with_fat()
    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    disk = machine_disk()
    slog = tempfile.mktemp(suffix=".log")
    check("the disc with the 64-bit kernel builds",
          make_iso64(d, iso, text=True))
    if not os.path.exists(iso):
        return
    q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk, sata=True,
             qemu="qemu-system-x86_64",
             devices=["-device", "usb-ehci,id=ehci",
                      "-drive", "if=none,id=st,format=raw,file=" + stick,
                      "-device", "usb-storage,bus=ehci.0,drive=st"])
    try:
        boot = q.wait_serial("thread64>", timeout=120)
        check("(64) the stick is found on EHCI",
              re.search(r"USB disk: .*\(port \d+, EHCI\)", boot) is not None)
        out = q.command("disks", "sectors", timeout=20)
        n = usb_disk_number(out)
        ok, said = crc_matches(q, stick, n, 2048, 256) if n else (False, "")
        check("(64) 128 KiB in one command, the CRC matches (%s)" % said, ok)
        q.command("fat 1 %d" % n, "cluster", timeout=30)
        q.command("fatput /long.txt from-long-mode", "fatput:", timeout=20)
        q.command("fat 1 %d" % n, "cluster", timeout=30)   # flush
    finally:
        q.close()
    got = fat_get(stick, start, "/long.txt")
    check("(64) a file written in long mode, read back by mtools",
          got is not None and b"from-long-mode" in got)
    for f in (stick, disk, slog, iso):
        if os.path.exists(f):
            os.unlink(f)


def main():
    print()
    if not mtools_ok():
        print("SKIPPED: no mtools - nothing to check the stick with")
        return summary()
    stick_alone()
    board_layout()
    fast_keyboard()
    hotplug()
    long_mode()
    return summary()


sys.exit(main())
