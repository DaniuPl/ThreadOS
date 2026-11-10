#!/usr/bin/env python3
"""ThreadOS smoke test: an NVMe disk (PCI Express).

The block layer has existed since the days of AHCI: the file system and
page swapping do not know which controller sits underneath. NVMe is the
third driver under that same name and the least like the rest - there is
nothing of the ATA heritage in it. Instead of registers and FIS frames
there are two queues in memory: we put commands in, the controller puts
answers back, and a phase bit says that something has returned.

We check that the way a disk is checked: we write, we read, and then
start the machine a SECOND TIME on the same image. A file that survived
a restart really did lie on the platter and not in memory.
"""

import os
import re
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

PROMPT = "thread64>"
TEXT = "content-through-the-queues"
# What user64/fileio64.c writes into 'zring3'
RING3 = "ThreadOS 64: a file written from ring 3"
# The last 32 KiB of the image, read back in one 'disks read'
TAIL_SECTORS = 64
TAIL_LBA = DISK_SIZE // 512 - TAIL_SECTORS


def read_crc(q, disk_no, lba, count, timeout=30):
    """'disks read' and the CRC-32 it printed, or None. Waited for up to
    the comma after the eight digits: the line reaches the port a
    character at a time."""
    start = len(q.serial())
    q.type_line("disks read %d %d %d" % (disk_no, lba, count))
    end = time.time() + timeout
    while time.time() < end:
        m = re.search(r"crc32 ([0-9a-f]{8}),", q.serial()[start:])
        if m:
            return int(m.group(1), 16)
        time.sleep(0.3)
    return None


def main():
    print("--- an NVMe disk ---")
    disk = tempfile.mktemp(suffix=".img")
    tail = os.urandom(TAIL_SECTORS * 512)
    with open(disk, "wb") as f:
        f.write(b"\0" * (DISK_SIZE - len(tail)))
        f.write(tail)
    slog = tempfile.mktemp(suffix=".log")

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog, nvme=True)
    try:
        # We wait for the WHOLE line, not for the "TFS" tag alone: that
        # appears already when the disk is formatted, so waiting for it
        # was a race with the driver's message
        start = q.wait_serial("+ TFS ready", timeout=60)
        check("the block layer picked NVMe instead of ATA",
              "Disk NVMe (PCIe, queues) + TFS ready" in start)
        check("the filesystem formatted an empty NVMe disk",
              "TFS: an empty disk formatted" in start)

        # Page swapping creates its file only after the file system,
        # so we ask about it only now - and wait for the end of its
        # line, which reaches the port a character at a time
        swap = q.wait_serial("on the disk (swap.sys)", timeout=20)
        check("the swap file was created on the NVMe disk",
              re.search(r"Page swapping: \d+ KiB on the disk", swap)
              is not None)

        # The shell starts only after the scheduler, and keys typed
        # earlier are lost - that is not a thing to guess at
        q.wait_serial(PROMPT, timeout=30)
        q.wait_serial("DHCP: granted", timeout=40)

        write_out = q.command("write nvmetest " + TEXT, "write:")
        check("writing a file to the NVMe",
              "write: 'nvmetest' written" in write_out)
        read_res = q.command("cat nvmetest", TEXT)
        check("reading the same file gives the same bytes back",
              TEXT in read_res)

        # A ring 3 process by the same path: syscall -> TFS -> NVMe
        ring3 = q.command("run fileio64", "[fileio64] done, exit code",
                          timeout=40)
        check("a ring 3 process writes and reads a file over NVMe",
              "[fileio64] written and read back match byte for byte"
              in ring3 and "[fileio64] done, exit code 0" in ring3)

        # A program from the NVMe disk: the ELF loader reads it through
        # TFS, the same path as 'cat'. TFS goes a 1 KiB block at a
        # time, so every one of those is a two-sector command.
        q.command("cp hello64 znvme", "cp:")
        run_out = q.command("run znvme", "[hello64] ending", timeout=40)
        check("a program started straight off the NVMe disk",
              "Program 'znvme' from the disk" in run_out)

        # One long read: 64 sectors asked for at once. The driver's
        # DMA buffer is a page, so it splits them into eight commands,
        # each going on from the sector and the place in the caller's
        # buffer where the last one stopped - a loop nothing above
        # runs more than once. The bytes are random and the host put
        # them at the end of the image, where TFS on a disk this empty
        # never allocates; zlib's CRC of them is the judge.
        listing = q.command("disks", "sectors of 512 B", timeout=20)
        m = re.search(r"disk (\d+): NVMe", listing)
        got = read_crc(q, int(m.group(1)), TAIL_LBA, TAIL_SECTORS) \
            if m else None
        want = zlib.crc32(tail) & 0xFFFFFFFF
        check("a read of %d sectors in page-sized commands brings the "
              "right bytes (%s vs %08x)"
              % (TAIL_SECTORS, "%08x" % got if got is not None else None,
                 want),
              got == want)
    finally:
        q.close()
        if os.path.exists(slog):
            os.unlink(slog)

    # --- A second boot on the same image ---
    print("--- NVMe: a second boot, the same image ---")
    slog2 = tempfile.mktemp(suffix=".log")
    q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog2, nvme=True)
    try:
        start = q2.wait_serial("+ TFS ready", timeout=60)
        check("the second boot: the disk is formatted already",
              "Disk NVMe (PCIe, queues) + TFS ready" in start and
              "formatted" not in start)
        q2.wait_serial(PROMPT, timeout=30)
        q2.wait_serial("DHCP: granted", timeout=40)
        read_res = q2.command("cat nvmetest", TEXT)
        check("the file survived the restart - it really was on the disk",
              TEXT in read_res)
        ring3 = q2.command("cat zring3", RING3)
        check("the file written by ring 3 survived too",
              RING3 in ring3)
        run_out = q2.command("run znvme", "[hello64] ending", timeout=40)
        check("and so did the program copied onto it - it still starts",
              "Program 'znvme' from the disk" in run_out and
              "[hello64] ending" in run_out)
    finally:
        q2.close()
        for f in (disk, slog2):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
