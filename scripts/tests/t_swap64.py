#!/usr/bin/env python3
"""ThreadOS smoke test: swapping pages out to disk in the 64-bit kernel.

The program takes more memory than the machine has RAM. Along the way
the kernel has to throw some of its pages out to disk and fetch them
back when the program reaches for them - and the program must not notice
it other than as a moment's delay: every byte is to come back as it was.

We check both: the program confirms that all the pages came back whole,
and the kernel shows in 'mem' how many of them really went out to disk.
Without that second assertion the test would pass even if the memory
simply fitted and swapping never started once.

The store (kernel/swap.c) is shared with the 32-bit kernel. In long mode
an eviction policy was added to it: a walk of four levels of page tables
with the 'accessed' bit, and an entry for a page on disk which the
processor never looks at, because it has no present bit in it.
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

RAM_MIB = 16          # less than the program is about to take
# More than the RAM, but not more than the RAM left free plus the swap
# file (8 MiB): what the kernel and the initrd hold in memory is not
# free, and the initrd grows - a megabyte of JavaScript engine went into
# it, and 18 MiB stopped fitting
REQUEST_MIB = 17


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
            q.type_line("run swaptest64 %d" % REQUEST_MIB)
            out = q.wait_serial("[swaptest64] all", timeout=240)
            check("the program got more memory than the machine has RAM",
                  "[swaptest64] writing %d MiB" % REQUEST_MIB in out)
            check("every page came back off the disk unchanged",
                  "pages came back whole" in out)

            # Without this the test would pass even if swapping never
            # started at all
            q.type_line("mem")
            stan = q.wait_serial("Swap:", timeout=20)
            m = re.search(r"Swap: \d+/\d+ pages on disk "
                          r"\(evicted (\d+), fetched (\d+)\)", stan)
            check("the kernel really did evict pages to the disk",
                  m is not None and int(m.group(1)) > 0)
            check("and fetched them back",
                  m is not None and int(m.group(2)) > 0)

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
            q.type_line("run swaptest64 %d" % REQUEST_MIB)
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
