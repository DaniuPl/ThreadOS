#!/usr/bin/env python3
"""ThreadOS smoke test: memory above the low gigabyte.

The kernel managed 512 MiB of RAM at most, whatever the machine had:
it reached every frame at its own address, and programs lie at 1 GiB.
Now it manages up to 4 GiB. The low gigabyte stays identity-mapped for
what only it can hold - page tables, the heap, buffers devices are
given by address - and the frames above go to programs' pages, which
the kernel reaches through a second map of all memory in the top half
of the address space (kernel/pmm.c, kernel64/paging64.h).

On a machine with 3 GiB:

  'mem' says nearly all of it is free, not 512 MiB of it

  libc/tests/bigram.c writes 2 GiB - twice the low gigabyte - with
  words of each page's own and reads every page back; a forked child
  reads it all and writes into every 32nd page, so copies on write go
  from high frames to high frames, and the parent's memory stays as it
  was; a memfd of 256 MiB written through a mapping reads back through
  read(); the machine had the 2 GiB in use at once

  the machine has all of it back afterwards

and on a machine with 1.5 GiB, where the firmware's ACPI tables lie in
the gigabyte that is a program's address range, ACPI still reads them
(through the second map) and a program runs beside them.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

WANT = [
    "2048 MiB written, every page read back intact: yes",
    "the machine had it all in memory at once: yes",
    "a forked child read it all, and its writes (16384 pages copied) "
    "read back: yes",
    "and the parent's memory is as it was: yes",
    "a memfd of 256 MiB written through a mapping reads back through "
    "read(): yes",
]


def free_kib(text):
    m = re.search(r"\((\d+) KiB free\)", text)
    return int(m.group(1)) if m else None


def machine(mib, cpus, work):
    disk = os.path.join(work, f"disk{mib}.img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    return Qemu(KERNEL, INITRD, disk, serial=os.path.join(work, f"s{mib}.log"),
                ram_mib=mib, cpus=cpus, append="nodesktop")


def three_gib(work):
    q = machine(3072, 4, work)
    try:
        check("(3 GiB) the system came up",
              q.wait_serial("The 64-bit system is ready", timeout=120))
        before = free_kib(q.command("mem", "free)", timeout=20))
        check(f"(3 GiB) 'mem' says nearly all of it is free, not 512 MiB "
              f"({before} KiB)", before is not None and before > 2900 * 1024)
        out = q.command("run bigram 2048", "[bigram] done, exit code",
                        timeout=600)
        got = [l.strip() for l in out.replace("\r", "").split("\n")
               if l.strip().endswith((": yes", ": no", "refused"))]
        check(f"(3 GiB) bigram says yes to every question "
              f"({[g for g in got if g not in WANT] or 'all yes'})",
              got == WANT)
        check("(3 GiB) and ends with code 0",
              "[bigram] done, exit code 0" in out)
        for l in out.replace("\r", "").split("\n"):
            if l.strip().startswith("~"):
                print("   ", l.strip())
        after = free_kib(q.command("mem", "free)", timeout=20))
        check(f"(3 GiB) the machine has all of it back afterwards "
              f"({before} -> {after} KiB free)",
              before is not None and after is not None and
              abs(before - after) < 256)
    finally:
        q.close()


def acpi_in_the_programs_gigabyte(work):
    q = machine(1536, None, work)
    try:
        check("(1.5 GiB) the system came up",
              q.wait_serial("The 64-bit system is ready", timeout=120))
        log = q.serial()
        rsdt = re.search(r"ACPI: RSDT 0x([0-9A-F]+)", log)
        at = int(rsdt.group(1), 16) if rsdt else 0
        check(f"(1.5 GiB) the ACPI tables lie in the gigabyte at 1 GiB, a "
              f"program's addresses (RSDT at {at:#x})",
              (1 << 30) <= at < (2 << 30))
        check("(1.5 GiB) and ACPI read them: the firmware's AML runs",
              "the firmware's AML runs" in log)
        out = q.command("run bigram 512", "[bigram] done, exit code",
                        timeout=300)
        check("(1.5 GiB) a program fills 512 MiB beside them and ends with "
              "code 0", "[bigram] done, exit code 0" in out and
              "512 MiB written, every page read back intact: yes" in out)
    finally:
        q.close()


def main():
    print("--- memory above the low gigabyte ---")
    work = tempfile.mkdtemp()
    try:
        three_gib(work)
        acpi_in_the_programs_gigabyte(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
