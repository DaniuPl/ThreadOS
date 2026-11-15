#!/usr/bin/env python3
"""ThreadOS smoke test: page faults without the big kernel lock.

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
a timing measurement can only suggest: with 'runw tlbtest64' the
number served WITHOUT the lock grows by the number of pages touched, and
the number that needed it does not move at all. tlbtest64 is four
threads asking for fresh memory in one address space, so their faults
meet on several cores at once.

Correctness is asserted just as hard, and it is the reason the counters
are worth anything: every page has to come back holding what was
written into it. A frame handed out twice shows up as a wrong pattern.

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

# tlbtest64 touches 4 threads x 10 rounds x 4 pages of fresh memory,
# all in one address space shared by the threads
PAGES = 4 * 10 * 4
ROW = re.compile(r"\[tlbtest64\] threads \d+, chunks (\d+) of \d+ pages, "
                 r"core mask (\d+)")
COUNTERS = re.compile(r"Page faults: without the lock (\d+), with it (\d+)")


def faults(q, timeout=60):
    """(served without the big lock, served with it)."""
    m = COUNTERS.search(q.command("smp", "Page faults:", timeout=timeout))
    return tuple(int(x) for x in m.groups()) if m else None


def run_case(q, cmd, label, timeout=240):
    """The core mask the threads ran on, or None. Every page has to come
    back holding what was written into it - a frame handed out twice
    shows up as a wrong pattern."""
    # 'runw' gives the prompt back at once and says nothing when the
    # program ends, so what is waited for is the program's own verdict
    start = len(q.serial())
    q.type_line(cmd)
    deadline = time.time() + timeout
    out = ""
    while time.time() < deadline:
        out = q.serial()[start:]
        if "the memory agrees on every core" in out or "WRONG" in out:
            break
        time.sleep(0.3)
    if cmd.startswith("run "):
        # ...and 'run' holds the shell until it has reaped the program
        end = time.time() + 30
        while "exit code" not in q.serial()[start:] and time.time() < end:
            time.sleep(0.3)
        out = q.serial()[start:]
    m = ROW.search(out)
    check("the program reported back%s" % label, m is not None)
    check("every page came back holding what was written into it%s"
          % label, "the memory agrees on every core" in out
          and "WRONG" not in out)
    return int(m.group(2)) if m else None


def settled(q, read):
    """The counter after it has stopped moving.

    A process that has reported its result is not necessarily finished:
    the last threads are still ending, and their faults are still being
    counted. Reading the counter one moment too early makes the
    difference look smaller than it was - which on a loaded host is a
    failure that says nothing about the kernel. So we look until two
    readings agree."""
    previous = None
    for _ in range(12):
        now = read(q)
        if now is not None and now == previous:
            return now
        previous = now
        time.sleep(1.0)
    return previous


def main():
    print("--- page faults without the big lock ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    slog1 = tempfile.mktemp(suffix=".log")
    try:
        check("the disc with the 64-bit kernel builds", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, cpus=4,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=90)
            before = faults(q)
            check("the kernel counts page faults", before is not None)

            # --- All the threads held on the boot core ----------------
            mask = run_case(q, "run tlbtest64", " ('run')")
            mid = settled(q, faults)
            if mask is not None and before and mid:
                check("without 'runw' every thread stays on the boot core "
                      "(mask %d)" % mask, mask == 1)
                grew = mid[0] - before[0]
                check("faults served without the lock (%d, pages %d)"
                      % (grew, PAGES), grew >= PAGES)
                check("and not one had to take it (%d)"
                      % (mid[1] - before[1]), mid[1] == before[1])

            # --- Allowed anywhere -------------------------------------
            mask = run_case(q, "runw tlbtest64", " ('runw')")
            after = settled(q, faults)
            if mask is not None and mid and after:
                check("with 'runw' they run off the boot core too "
                      "(mask %d)" % mask, (mask & ~1) != 0)
                # Here the threads fault on several cores at once, and
                # the count is one locked addition each: not one may go
                # missing
                grew = after[0] - mid[0]
                check("and again without the lock (%d, pages %d)"
                      % (grew, PAGES), grew >= PAGES)
                check("still not one with the lock (%d)"
                      % (after[1] - mid[1]), after[1] == mid[1])

            # The 'runw' program is the shell's child too, and nobody
            # waits for it: 'run' once took its exit for its own
            check("after it 'run' still waits for its own program "
                  "(exit code 7)", "[hello64] done, exit code 7" in
                  q.command("run hello64", "done, exit code", timeout=60))
        finally:
            q.close()

        print("--- the same attempt on one core ---")
        q1 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog1,
                  qemu="qemu-system-x86_64")
        try:
            q1.wait_serial("thread64>", timeout=90)
            before = faults(q1)
            mask = run_case(q1, "runw tlbtest64", " (one core)")
            after = settled(q1, faults)
            if mask is not None and before and after:
                check("on one core the mask is 1 (%d)" % mask, mask == 1)
                check("and the faults still go without the lock (%d, "
                      "pages %d)" % (after[0] - before[0], PAGES),
                      after[0] - before[0] >= PAGES)
                check("and not one with it (%d)" % (after[1] - before[1]),
                      after[1] == before[1])
        finally:
            q1.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, slog1):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
