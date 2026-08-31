#!/usr/bin/env python3
"""ProtonOS smoke test: page faults without the big kernel lock.

A page fault is the commonest way into the kernel there is. Every
program that touches memory goes through it thousands of times, and
most of those faults are not errors and barely work: a frame from the
allocator, four kilobytes of zeros and one entry in a table. All three
of those have locks of their own.

Until now every one of them took the big kernel lock anyway, so two
processes touching their own memory, in their own address spaces, with
nothing shared between them, still went one after the other.

Now the causes that reach neither the disk nor the task list - heap,
stack, anonymous memory in the mapping area, and copy-on-write - are
served before the lock is taken. The list is short and explicit, like
the list of system calls that need no lock, and the default answer is
still "with the lock".

What is asserted here is the mechanism, not a stopwatch. The kernel
counts the faults it served each way, and that count says outright what
a timing measurement can only suggest: with 'runw faults' the number
served WITHOUT the lock grows by the number of pages touched, and the
number that needed it does not move at all.

Correctness is asserted just as hard, and it is the reason the counters
are worth anything: every page has to come back holding what was
written into it, and a fresh page has to be zeroed. A frame handed to
two processes at once shows up as a wrong sum.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64

ROW = re.compile(r"\[faults\] (dziecko|rodzic ): rdzenie (\d+), tyknac (\d+), "
                 r"stron (\d+), suma (\d+)")
COUNTERS = re.compile(r"Bledy stron: bez zamka (\d+), z zamkiem (\d+)")


def faults(q, timeout=60):
    """(served without the big lock, served with it)."""
    m = COUNTERS.search(q.command("smp", "Bledy stron:", timeout=timeout))
    return tuple(int(x) for x in m.groups()) if m else None


def run_case(q, cmd, label, timeout=240):
    """(mask of both processes, slowest one's ticks, pages, sums)."""
    rows = ROW.findall(q.command(cmd, "gotowe", timeout=timeout))
    check("oba procesy zdaly raport%s (%d z 2)" % (label, len(rows)),
          len(rows) == 2)
    if len(rows) != 2:
        return None
    mask = 0
    for r in rows:
        mask |= int(r[1])
    ticks = max(int(r[2]) for r in rows)
    pages = int(rows[0][3])
    sums = [int(r[4]) for r in rows]

    # Two processes doing the same work have to get the same answer -
    # and a frame handed to both would not give one
    check("oba policzyly to samo (%d)%s" % (sums[0], label),
          sums[0] == sums[1] and sums[0] > 0)
    return mask, ticks, pages, sums[0]


def main():
    print("--- bledy stron bez wielkiego zamka (jadro 32-bitowe) ---")
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog, cpus=4)
        try:
            q.wait_for("ProtonOS shell", timeout=40)

            before = faults(q)
            check("jadro liczy bledy stron", before is not None)

            # --- Both processes held on the boot core -----------------
            res = run_case(q, "run faults", " ('run')")
            mid = faults(q)
            if res:
                mask, ticks, pages, _ = res
                check("bez 'runw' oba zostaja na rdzeniu rozruchowym "
                      "(maska %d)" % mask, mask == 1)
                # 2 processes x 'pages' first touches, and not one of
                # them was allowed to close the kernel for the other.
                # 'Essentially all' rather than 'exactly all': a page
                # can turn out to be there already - mapped when the
                # heap was moved, or touched before the loop began -
                # and a fault that never happens counts nowhere. The
                # assertion that has to stay exact is the next one.
                grew = mid[0] - before[0]
                check("bledy obsluzone bez zamka (%d, stron 2 x %d)"
                      % (grew, pages), grew >= 2 * pages - 16)
                check("i ani jeden nie musial go wziac (%d)"
                      % (mid[1] - before[1]), mid[1] == before[1])

            # --- Allowed anywhere -------------------------------------
            res = run_case(q, "runw faults", " ('runw')")
            after = faults(q)
            if res:
                mask, ticks, pages, _ = res
                check("z 'runw' bieglyby takze poza rdzeniem rozruchowym "
                      "(maska %d)" % mask, (mask & ~1) != 0)
                grew = after[0] - mid[0]
                check("i znowu bez zamka (%d, stron 2 x %d)"
                      % (grew, pages), grew >= 2 * pages - 16)
                check("nadal ani jednego z zamkiem (%d)"
                      % (after[1] - mid[1]), after[1] == mid[1])

            check("system dziala dalej po probie",
                  "zakonczyl sie (kod 0)" in
                  q.command("run hello", "zakonczyl sie", timeout=60))
        finally:
            q.close()
    finally:
        if os.path.exists(slog):
            os.unlink(slog)

    print("--- ta sama proba na jednym rdzeniu ---")
    slog1 = tempfile.mktemp(suffix=".log")
    try:
        q1 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog1)
        try:
            q1.wait_for("ProtonOS shell", timeout=40)
            before = faults(q1)
            res = run_case(q1, "runw faults", " (jeden rdzen)")
            after = faults(q1)
            if res:
                mask, ticks, pages, _ = res
                check("na jednym rdzeniu maska to 1 (%d)" % mask, mask == 1)
                check("a bledy i tak ida bez zamka (%d, stron 2 x %d)"
                      % (after[0] - before[0], pages),
                      after[0] - before[0] >= 2 * pages - 16)
        finally:
            q1.close()
    finally:
        if os.path.exists(slog1):
            os.unlink(slog1)

    # The same split in long mode. There is no 'faults' program there,
    # but tlbtest64 touches four hundred fresh pages in four threads -
    # which is the same path and asks the same question of it.
    print("--- to samo w jadrze 64-bitowym ---")
    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog64 = tempfile.mktemp(suffix=".log")
    try:
        check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog64, cpus=4,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=90)
            before = faults(q)
            check("jadro 64-bitowe tez liczy bledy stron", before is not None)
            out = q.command("runw tlbtest64", "kod wyjscia", timeout=180)
            check("program przeszedl (64-bit)",
                  "pamiec zgodna na wszystkich rdzeniach" in out)
            after = faults(q)
            check("bledy obsluzone bez zamka (64-bit, %d)"
                  % (after[0] - before[0]), after[0] - before[0] >= 160)
            check("i ani jeden nie musial go wziac (64-bit, %d)"
                  % (after[1] - before[1]), after[1] == before[1])
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog64):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
