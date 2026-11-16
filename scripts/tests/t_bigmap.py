#!/usr/bin/env python3
"""ThreadOS smoke test: the mapping area at 512 GiB.

A program's mappings - anonymous memory, files, shared memory, the
stacks of its threads - used to share its one gigabyte with its image,
heap and stack, a quarter of it theirs: 252 MiB. A web engine reserves
more before it reads a page. They lie at 512 GiB now, with 64 GiB of
room (kernel64/elf64.h, PAGING64_MMAP_PML4). libc/tests/bigmap.c says
what it found, on one core and on four:

  three mappings of a gigabyte each, all above 512 GiB, a byte every
  16 MiB written and read back intact

  madvise(MADV_DONTNEED) gives pages back, and they read as zeros

  a forked child sees the parent's memory there, and what it writes
  stays its own: the kernel copies the area's tables on fork

  four threads, whose stacks are mappings too, run and come back

and the machine has all its memory back once the program ends.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

WANT = [
    "three gigabytes mapped, above 512 GiB: yes",
    "a byte every 16 MiB read back intact: yes",
    "madvise(MADV_DONTNEED) reads back zeros: yes",
    "a forked child sees the parent's memory there: yes",
    "and what it writes stays its own: yes",
    "four threads ran and came back: yes",
]


def one(cpus):
    work = tempfile.mkdtemp()
    disk = os.path.join(work, "disk.img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    q = Qemu(KERNEL, INITRD, disk, serial=os.path.join(work, "s.log"),
             cpus=cpus if cpus > 1 else None, append="nodesktop")
    try:
        q.wait_serial("The 64-bit system is ready", timeout=90)
        before = q.command("mem", "free)", timeout=20)
        out = q.command("run bigmap", "[bigmap] done, exit code", timeout=120)
        after = q.command("mem", "free)", timeout=20)
        got = [l.strip() for l in out.replace("\r", "").split("\n")
               if ": yes" in l or ": no" in l or "refused" in l]
        check(f"({cpus} core(s)) bigmap says yes to every question "
              f"({[g for g in got if g not in WANT] or 'all yes'})",
              got == WANT)
        check(f"({cpus} core(s)) and ends with code 0",
              "[bigmap] done, exit code 0" in out)
        free = [int(m) for m in
                (re.search(r"\((\d+) KiB free\)", t).group(1)
                 for t in (before, after) if re.search(r"\((\d+) KiB free\)", t))]
        check(f"({cpus} core(s)) the machine has its memory back afterwards "
              f"({free} KiB free)",
              len(free) == 2 and abs(free[0] - free[1]) < 256)
    finally:
        q.close()


def main():
    print("--- the mapping area at 512 GiB ---")
    one(1)
    one(4)
    return summary()


sys.exit(main())
