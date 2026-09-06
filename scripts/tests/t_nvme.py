#!/usr/bin/env python3
"""ProtonOS smoke test: an NVMe disk (PCI Express).

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
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def main():
    # --- The 32-bit kernel ---
    print("--- an NVMe disk (the 32-bit kernel) ---")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    slog = tempfile.mktemp(suffix=".log")

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog, nvme=True)
    try:
        start = q.wait_for("+ PFS ready", timeout=60)
        check("the block layer picked NVMe instead of ATA",
              "Disk NVMe (PCIe, queues) + PFS ready" in start)
        check("the filesystem formatted an empty NVMe disk",
              "formatted" in start)
        # The shell starts only after the scheduler, and keys typed
        # earlier are lost - that is not a thing to guess at
        q.wait_for("ProtonOS shell", timeout=30)
        q.wait_for("[net] DHCP", timeout=30)

        # Page swapping creates its file only after the file system,
        # so we ask about it only now
        check("the swap file was created on the NVMe disk",
              re.search(r"Page swapping: \d+ KiB",
                        q.wait_serial("Page swapping:", timeout=20))
              is not None)

        write_out = q.command("write nvmetest content-through-the-queues", "wrote")
        check("writing a file to the NVMe", "wrote" in write_out)
        read_res = q.command("cat nvmetest", "content-through-the-queues")
        check("reading the same file gives the same bytes back",
              "content-through-the-queues" in read_res)

        # A ring 3 process by the same path: syscall -> PFS -> NVMe
        ring3 = q.command("run fileio", "[fileio] OK", timeout=40)
        check("a ring 3 process writes and reads a file over NVMe",
              "read off the disk: a file written by a ring 3 process" in ring3)

        # A large file: a dozen or so batches of one page each - here
        # it can be seen whether the loop splitting the transfer counts
        # correctly
        big = q.command("run memtest", "[memtest]", timeout=60)
        check("the system is alive after heavy disk traffic",
              "[memtest]" in big)
    finally:
        q.close()
        if os.path.exists(slog):
            os.unlink(slog)

    # --- A second boot on the same image ---
    print("--- NVMe: a second boot, the same image ---")
    slog2 = tempfile.mktemp(suffix=".log")
    q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog2, nvme=True)
    try:
        start = q2.wait_for("+ PFS ready", timeout=60)
        check("the second boot: the disk is formatted already",
              "formatted" not in start)
        q2.wait_for("ProtonOS shell", timeout=30)
        q2.wait_for("[net] DHCP", timeout=30)
        read_res = q2.command("cat nvmetest", "content-through-the-queues")
        check("the file survived the restart - it really was on the disk",
              "content-through-the-queues" in read_res)
        listing = q2.command("fls", "ring3.txt")
        check("the file written by ring 3 survived too",
              "ring3.txt" in listing)
    finally:
        q2.close()
        for f in (disk, slog2):
            if os.path.exists(f):
                os.unlink(f)

    # --- The 64-bit kernel ---
    print("--- an NVMe disk (the 64-bit kernel) ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog64 = tempfile.mktemp(suffix=".log")
    disk64 = tempfile.mktemp(suffix=".img")
    with open(disk64, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    try:
        check("the disc with the 64-bit kernel builds", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        q64 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog64, disk=disk64,
                   nvme=True, qemu="qemu-system-x86_64")
        try:
            # We wait for the WHOLE line, not for the "PFS" tag alone:
            # that appears already when the disk is formatted, so waiting
            # for it was a race with the driver's message
            start = q64.wait_serial("+ PFS ready", timeout=90)
            check("the 64-bit kernel sees the NVMe disk too",
                  "Disk NVMe (PCIe, queues) + PFS ready" in start)
            q64.wait_serial("proton64>", timeout=30)

            q64.command("write file64 content-from-long-mode", "write:")
            read_res = q64.command("cat file64", "content-from-long-mode")
            check("writing and reading over NVMe in long mode",
                  "content-from-long-mode" in read_res)

            # A program from the NVMe disk: the ELF loader reads it by the
            # same path
            q64.command("cp hello64 znvme", "cp:")
            run_out = q64.command("run znvme", "[hello64] ending",
                                  timeout=40)
            check("a program started straight off the NVMe disk",
                  "Program 'znvme' from the disk" in run_out)
        finally:
            q64.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog64, disk64):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
