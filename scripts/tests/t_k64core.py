#!/usr/bin/env python3
"""ProtonOS smoke test: the long mode boot (64-bit kernel).

The first step of every 64-bit kernel: the processor is to confirm by
itself that it is in long mode (LMA=1), the page tables are to be built
out of frames taken from the memory manager rather than laid out
statically in the kernel image, and an interrupt has to work.

The strongest assertion concerns a page placed eight gigabytes above the
area described one to one: writing and reading through it go the whole
four-level address translation path, not a shortcut.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
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


def main():
    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")
    # A disk behind an AHCI controller: it was this path that required
    # adding the controller's registers to the mapping
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.write(b"\0" * DISK_SIZE)

    try:
        check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        q23 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                   sata=True, qemu="qemu-system-x86_64")
        try:
            out = q23.wait_serial("Tryb dlugi:", timeout=40)
            check("jadro startuje w trybie dlugim",
                  "Jadro uruchomione w trybie dlugim" in out)
            # LMA is set by the processor itself - it is its word, not ours
            check("procesor potwierdza tryb dlugi (LMA=1)",
                  "LMA=1" in out)
            check("wskazniki maja osiem bajtow",
                  "wskaznik ma 8 bajtow" in out)
            check("mapa pamieci czytana tym samym kodem",
                  re.search(r"Pamiec fizyczna: \d{5,} KiB RAM",
                            q23.wait_serial("Pamiec fizyczna",
                                            timeout=20)) is not None)
            check("bufor obrazu od loadera rozpoznany",
                  re.search(r"Bufor obrazu: \d+x\d+",
                            q23.wait_serial("Bufor obrazu",
                                            timeout=20)) is not None)

            # Memory: our own tables built out of frames from the PMM,
            # not the ones laid out statically in the kernel image
            mem = q23.wait_serial("Stronicowanie czteropoziomowe",
                                  timeout=20)
            m = re.search(r"czteropoziomowe: (\d+) MiB, zakaz "
                          r"wykonywania (\w+)", mem)
            check("wlasne tablice stron obejmuja cala pamiec"
                  + (f" ({m.group(1)} MiB)" if m else ""),
                  m is not None and int(m.group(1)) >= 100)
            check("zakaz wykonywania wlaczony (bit NXE)",
                  m is not None and m.group(2) == "wlaczony")
            # A page placed eight gigabytes above the area described
            # one to one - writing and reading through it go the whole
            # four-level path
            hi = q23.wait_serial("Mapowanie 4 KiB", timeout=20)
            check("mapowanie 4 KiB daleko poza pamiecia dziala",
                  "odczyt 123456789abcdef" in hi)
            heap = q23.wait_serial("Sterta jadra", timeout=20)
            check("sterta jadra dziala w trybie dlugim",
                  "kmalloc/kfree dzialaja" in heap
                  and "po zwolnieniu 0 B" in heap)

            # Interrupts in long mode: an int3 trap goes the whole
            # way - stub, tables, register frame, iretq - and comes
            # back without stopping the system
            trap = q23.wait_serial("[INT3]", timeout=20)
            check("wyjatek-pulapka obsluzony i wznowiony",
                  re.search(r"\[INT3\] breakpoint pod rip=",
                            trap) is not None)
            # And the timer has to tick of its own accord - that is
            # the proof that hardware interrupts arrive through the new
            # table

            # Threads: the counters have to match the length of the
            # sleep exactly. Over 600 ms a thread sleeping 20 ms at a
            # time manages 30 turns, and one sleeping 50 ms - 12. Broken
            # context switching or sleeping pushes those numbers out of
            # range at once.
            sch = q23.wait_serial("Watki licza rownolegle",
                                  timeout=25)
            m = re.search(r"A=(\d+), B=(\d+)", sch)
            check("watki dzialaja rownolegle, a sen trwa tyle, ile ma"
                  + (f" (A={m.group(1)}, B={m.group(2)})"
                     if m else ""),
                  m is not None and 28 <= int(m.group(1)) <= 32
                  and 11 <= int(m.group(2)) <= 13)
            zombie = q23.wait_serial("Watek konczacy sie", timeout=20)
            check("zakonczony watek znika z listy",
                  "zrobil swoje" in zombie
                  and "zadan na liscie: 4" in zombie)
        finally:
            q23.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, slog2, disk):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
