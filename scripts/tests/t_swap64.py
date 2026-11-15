#!/usr/bin/env python3
"""ThreadOS smoke test: swapping pages out to disk in the 64-bit kernel.

The program takes more memory than the machine has free. Along the way
the kernel has to throw some of its pages out to disk and fetch them
back when the program reaches for them - and the program must not notice
it other than as a moment's delay: every byte is to come back as it was.

We check both: the program confirms that all the pages came back whole,
and the kernel shows in 'mem' how many of them really went out to disk.
Without that second assertion the test would pass even if the memory
simply fitted and swapping never started once.

The store (kernel/swap.c) has an eviction policy: a walk of four levels
of page tables with the 'accessed' bit, and an entry for a page on disk
which the processor never looks at, because it has no present bit in
it.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64

# The initrd lies in memory beside the kernel, and it grows: at 16 MiB
# GRUB could no longer load it at all, and the machine came up with no
# programs. So the machine is bigger, and what the program asks for is
# reckoned from what 'mem' says is free once the system is up.
RAM_MIB = 32
# How far beyond the free memory the program reaches: every page of it
# has to go out to the swap file (8 MiB) and back, with room to spare
OVER_MIB = 5


def main():
    print("--- swapping pages to the disk (the 64-bit kernel) ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(64 * 1024 * 1024)
    try:
        check("the disc with the 64-bit kernel builds", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        # A disk behind an AHCI controller: the pages go to it through DMA
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, ram_mib=RAM_MIB, qemu="qemu-system-x86_64")
        try:
            start = q.wait_serial("Page swapping:", timeout=90)
            check("the swap file was created on the disk",
                  re.search(r"Page swapping: \d+ KiB on the disk \(swap\.sys\)",
                            start) is not None)

            q.wait_serial("thread64>", timeout=30)
            m = re.search(r"\((\d+) KiB free\)",
                          q.command("mem", "thread64>", timeout=20))
            free_kib = int(m.group(1)) if m else RAM_MIB * 1024
            request = -(-free_kib // 1024) + OVER_MIB
            q.type_line("run swaptest64 %d" % request)
            out = q.wait_serial("[swaptest64] all", timeout=240)
            check("the program got more memory than the machine has free "
                  "(%d MiB, %d KiB free)" % (request, free_kib),
                  m is not None
                  and "[swaptest64] writing %d MiB" % request in out)
            check("every page came back off the disk unchanged",
                  "pages came back whole" in out)

            # Without this the test would pass even if swapping never
            # started at all
            # Only what this 'mem' says: the one above is on the port too
            stan = q.command("mem", "thread64>", timeout=20)
            m = re.search(r"Swap: \d+/\d+ pages on disk "
                          r"\(evicted (\d+), fetched (\d+)\)", stan)
            # At least the pages beyond the free memory went out - over
            # a thousand of them - and nearly every one came back in
            least = 1000
            check("the kernel really did evict pages to the disk (%s)"
                  % (m.group(1) if m else "-"),
                  m is not None and int(m.group(1)) > least)
            check("and fetched them back (%s)" % (m.group(2) if m else "-"),
                  m is not None and int(m.group(2)) > least)

            # After the program ends its space in the swap file returns to
            # the pool: cleaning the address space up frees the pages
            # that lay on the disk at the moment of death too
            used = re.search(r"Swap: (\d+)/", stan)
            check("once the program ends the swap file is empty",
                  used is not None and int(used.group(1)) == 0)

            # A second pass on the same machine: the space reclaimed really
            # can be used once more. At the end we wait for the SECOND
            # occurrence of the text, because the first hangs on the port
            # from the previous pass.
            q.type_line("run swaptest64 %d" % request)
            end = time.time() + 240
            second = ""
            while time.time() < end:
                second = q.serial()
                if second.count("pages came back whole") >= 2:
                    break
                time.sleep(1.0)
            check("a second attempt on the same machine passes too",
                  second.count("pages came back whole") == 2)
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, disk):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
