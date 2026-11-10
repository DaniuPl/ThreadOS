#!/usr/bin/env python3
"""ThreadOS smoke test: swapping pages out to disk.

The program takes more memory than the machine has RAM. Along the way
the kernel has to throw some of its pages out to disk and fetch them
back when the program reaches for them - and the program must not notice
it other than as a moment's delay: every byte is to come back as it was.

We check both: the program confirms that all the pages came back whole,
and the kernel shows in 'mem' how many of them really went out to disk.
Without that second assertion the test would pass even if the memory
simply fitted and swapping never started once. How many is enough is
not a guess: 'mem' says how many frames were free before the program
started, and every page it touched beyond those had to go to the disk
and, when it was read back, come back from there.

The store is kernel/swap.c; the eviction policy over it is a walk of
four levels of page tables with the 'accessed' bit, and an entry for a
page on disk which the processor never looks at, because it has no
present bit in it.

The initrd carries the browser and its JavaScript engine, and on a
machine this small it would not even fit beside the kernel - GRUB
quietly leaves it behind. So the program is copied onto the disk on a
first boot with the usual memory, and the machine that swaps starts with
no initrd at all and runs it from there.

The swap file is made anew at every start, as a quarter of the free
space. The last start's file must not count as taken when that is
measured, or the same disk gets a swap file of a different size at
every boot - so the disk is started once more at the end, and its swap
file has to come out exactly as big as it was.
"""

import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64

RAM_MIB = 16          # less than the program is about to take
# More than the RAM, but not more than the RAM left free plus the swap
# file: what the kernel holds in memory is not free, and the swap file
# is a quarter of the free space of the file system - about eight
# megabytes on this disk, at every boot of it
REQUEST_MIB = 17

SWAP_LINE = re.compile(r"Page swapping: (\d+) KiB on the disk \(swap\.sys\)")


def swap_kib(boot_log):
    """The size the kernel gave the swap file at this start, or None"""
    m = SWAP_LINE.search(boot_log)
    return int(m.group(1)) if m else None


def mem_stats(q, timeout=20):
    """What 'mem' says now: frames used, frames in all, pages in the swap
    file, pages evicted and pages fetched, or None. Only what this 'mem'
    printed counts - the earlier answers are still on the port - and
    the Swap line is waited for whole, since it arrives a character at a
    time."""
    start = len(q.serial())
    q.type_line("mem")
    end = time.time() + timeout
    while time.time() < end:
        fresh = q.serial()[start:]
        f = re.search(r"frames (\d+)/(\d+) used", fresh)
        s = re.search(r"Swap: (\d+)/\d+ pages on disk "
                      r"\(evicted (\d+), fetched (\d+)\)", fresh)
        if f and s:
            return tuple(int(v) for v in f.groups() + s.groups())
        time.sleep(0.3)
    return None


def main():
    print("--- swapping pages to the disk ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return 0

    d = tempfile.mkdtemp()
    d2 = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    bare = tempfile.mktemp(suffix=".iso")
    slog0 = tempfile.mktemp(suffix=".log")
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(64 * 1024 * 1024)
    try:
        check("the disc with the kernel builds", make_iso64(d, iso))
        check("the disc with the kernel and no initrd builds",
              make_iso64(d2, bare, initrd=""))
        if not os.path.exists(iso) or not os.path.exists(bare):
            return summary()

        # --- The program onto the disk ---
        q0 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog0, disk=disk,
                  sata=True)
        try:
            q0.wait_serial("thread64>", timeout=90)
            out = q0.command("cp swaptest64 swaptest64", "cp:", timeout=60)
            check("the program was copied from the initrd onto the disk",
                  re.search(r"cp: 'swaptest64' -> 'swaptest64' \(\d+ B\)",
                            out) is not None)
        finally:
            q0.close()

        # --- The small machine ---
        # A disk behind an AHCI controller: the pages go to it through DMA
        q = Qemu(KERNEL, None, cdrom=bare, serial=slog, disk=disk,
                 sata=True, ram_mib=RAM_MIB)
        try:
            # (the whole line: it reaches the port a character at a time)
            start = q.wait_serial("on the disk (swap.sys)", timeout=90)
            size1 = swap_kib(start)
            check("the swap file was created on the disk", size1 is not None)
            # What makes the rest possible: no initrd in the memory
            check("the small machine started with no initrd in its memory",
                  "the boot loader handed over no module" in start)

            q.wait_serial("thread64>", timeout=30)
            before = mem_stats(q)
            check("'mem' says how much memory is free before the program "
                  "starts (%s)" % (before,), before is not None)
            free = before[1] - before[0] if before else 0

            q.type_line("run swaptest64 %d" % REQUEST_MIB)
            out = q.wait_serial("pages came back whole", timeout=240)
            check("the program got more memory than the machine has RAM",
                  "[swaptest64] writing %d MiB" % REQUEST_MIB in out)
            m = re.search(r"\[swaptest64\] all (\d+) pages came back whole",
                          out)
            check("every page came back off the disk unchanged",
                  m is not None)
            pages = int(m.group(1)) if m else 0
            # Otherwise the bound below is zero or less, and says nothing
            check("the program touched more pages than there were free "
                  "frames (%d against %d)" % (pages, free), pages > free)

            # Without this the test would pass even if swapping never
            # started at all. At the end of the writing pass at most
            # 'free' of the program's pages can be in memory - fewer,
            # since its page tables and stack take frames too - so at
            # least the rest went to the disk; the reading pass then
            # touches every one of them, and each comes back by a fetch.
            after = mem_stats(q)
            need = pages - free
            evicted = after[3] - before[3] if after and before else 0
            fetched = after[4] - before[4] if after and before else 0
            check("the kernel really did evict pages to the disk "
                  "(%d, at least %d)" % (evicted, need),
                  after is not None and need > 0 and evicted >= need)
            check("and fetched them back (%d, at least %d)"
                  % (fetched, need),
                  after is not None and need > 0 and fetched >= need)

            # After the program ends its space in the swap file returns to
            # the pool: cleaning the address space up frees the pages
            # that lay on the disk at the moment of death too
            check("once the program ends the swap file is empty",
                  after is not None and after[2] == 0)

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

        # --- The same disk, started once more ---
        # Nothing has been written to it since the last start except
        # pages into the swap file's own blocks, so a quarter of the free
        # space is what it was then. Counting the old swap.sys as taken
        # would make this one a different size from the last.
        q2 = Qemu(KERNEL, None, cdrom=bare, serial=slog2, disk=disk,
                  sata=True, ram_mib=RAM_MIB)
        try:
            again = q2.wait_serial("on the disk (swap.sys)", timeout=90)
            size2 = swap_kib(again)
            check("the next start of the disk makes the swap file just as "
                  "big (%s KiB, then %s KiB)" % (size1, size2),
                  size1 is not None and size2 == size1)
        finally:
            q2.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        shutil.rmtree(d2, ignore_errors=True)
        for f in (iso, bare, slog0, slog, slog2, disk):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
