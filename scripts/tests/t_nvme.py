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
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def main():
    print("--- an NVMe disk ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog64 = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")
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
            # We wait for the WHOLE line, not for the "TFS" tag alone:
            # that appears already when the disk is formatted, so waiting
            # for it was a race with the driver's message
            start = q64.wait_serial("+ TFS ready", timeout=90)
            check("the block layer picked NVMe instead of ATA",
                  "Disk NVMe (PCIe, queues) + TFS ready" in start)
            check("the filesystem formatted an empty NVMe disk",
                  "TFS: an empty disk formatted" in start)
            q64.wait_serial("thread64>", timeout=30)

            # Page swapping creates its file only after the file
            # system, so we ask about it only now
            check("the swap file was created on the NVMe disk",
                  re.search(r"Page swapping: \d+ KiB",
                            q64.wait_serial("Page swapping:", timeout=20))
                  is not None)

            q64.command("write file64 content-from-long-mode", "write:")
            read_res = q64.command("cat file64", "content-from-long-mode")
            check("writing and reading over NVMe",
                  "content-from-long-mode" in read_res)

            # A ring 3 process by the same path: syscall -> TFS -> NVMe
            ring3 = q64.command("run fileio64", "[fileio64] done, exit code",
                                timeout=40)
            check("a ring 3 process writes and reads a file over NVMe",
                  "[fileio64] done, exit code 0" in ring3
                  and "match byte for byte" in ring3)

            # A large file: a hundred pages, written by a program and
            # read back through a mapping, which checks every byte -
            # here it can be seen whether the loop splitting the
            # transfer counts correctly
            q64.command("run mkmap64 big.txt 400", "exit code", timeout=120)
            big = q64.command("run mmapfile64 big.txt", "exit code",
                              timeout=120)
            check("a 400 KiB file comes back off the NVMe disk intact",
                  "the checked bytes match, and there were 409600" in big
                  and "WRONG" not in big)

            # A program from the NVMe disk: the ELF loader reads it by the
            # same path
            q64.command("cp hello64 znvme", "cp:")
            run_out = q64.command("run znvme", "[hello64] ending",
                                  timeout=40)
            check("a program started straight off the NVMe disk",
                  "Program 'znvme' from the disk" in run_out)
        finally:
            q64.close()

        # --- A second boot on the same image ---
        print("--- NVMe: a second boot, the same image ---")
        q2 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog2, disk=disk64,
                  nvme=True, qemu="qemu-system-x86_64")
        try:
            start = q2.wait_serial("+ TFS ready", timeout=90)
            check("the second boot: the disk is formatted already",
                  "formatted" not in start)
            q2.wait_serial("thread64>", timeout=30)
            read_res = q2.command("cat file64", "content-from-long-mode")
            check("the file survived the restart - it really was on the disk",
                  "content-from-long-mode" in read_res)
            read_res = q2.command("cat zring3", "written from ring 3")
            check("the file written by ring 3 survived too",
                  "ThreadOS 64: a file written from ring 3" in read_res)
        finally:
            q2.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog64, slog2, disk64):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
