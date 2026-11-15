#!/usr/bin/env python3
"""ThreadOS smoke test: telling the other cores that a mapping changed.

A page table is memory and memory is shared. A translation worked out
FROM that table is not: every core keeps its own, in a cache no
instruction of the kernel's can reach. Changing an entry therefore
changes what the tables say and nothing about what another core
believes.

Nothing suffered from that while a process ran on one core at a time.
Threads share an address space, so they are the first thing to walk
into it - and until now they were kept on the boot core for exactly
that reason. Now they are not, and the kernel asks: it sends an
interrupt to every core standing in the space and WAITS for the answer
before it goes on to free the frame.

We check four things, and the first two are what make the rest mean
anything:

  1. Threads of ONE process really did run on several cores. Without
     that the run says nothing at all, so the mask is an assertion and
     not a note.
  2. The mechanism really fired - the kernel counts the changes that
     had to reach another core, and that number has to grow.
  3. Memory is consistent. Every thread checks its own chunks, and the
     main thread checks ALL of them at the end, once the neighbours
     have had time to be handed memory somebody else was still using.
  4. It costs nothing where there is nobody to tell. 'run tlbtest64'
     keeps the process on the boot core and must not send a single
     interrupt; on a single-core machine neither must 'runw'.

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

HEAD = re.compile(r"\[tlbtest64\] threads (\d+), chunks (\d+) of (\d+) "
                  r"pages, core mask (\d+)")
BAD = re.compile(r"\[tlbtest64\] errors while writing (\d+), "
                 r"afterwards (\d+)")
COUNTERS = re.compile(r"TLB: local (\d+), remote (\d+), replies (\d+)")


def counters(q, wait, timeout=60):
    """(local only, sent to other cores, answers collected).

    Asked more than once on purpose: the counters are read with an
    ordinary shell command, and on a host running the whole suite at
    once the answer sometimes does not arrive inside the first
    timeout. A test that then dies of a missing value teaches nothing
    about the kernel."""
    for _ in range(3):
        m = COUNTERS.search(q.command("smp", wait, timeout=timeout))
        if m:
            return tuple(int(x) for x in m.groups())
        time.sleep(1.0)
    return None


def run_case(q, out, label):
    """What the program itself reports: (mask, chunks, early, late).

    'out' is what arrived after the command. When the wait for it ran
    out before the report did, the report is taken from the whole serial
    log instead - the LAST one in it, which is the run just made."""
    h = HEAD.search(out)
    b = BAD.search(out)
    if not h or not b:
        log = q.serial()
        hits = list(HEAD.finditer(log))
        bits = list(BAD.finditer(log))
        h = hits[-1] if hits else None
        b = bits[-1] if bits else None
    check("tlbtest reported back%s" % label, bool(h) and bool(b))
    if not h or not b:
        return None
    threads, chunks, pages, mask = (int(x) for x in h.groups())
    early, late = (int(x) for x in b.groups())

    # A run that mapped nothing would pass every other check here
    check("every thread got its chunks (%d of %d)%s"
          % (chunks, threads * 10, label), chunks == threads * 10)
    check("the memory agrees while writing (%d errors)%s" % (early, label),
          early == 0)
    # The one that matters: read after every thread has finished, so
    # after the neighbours had time to be given somebody else's frames
    check("the memory agrees after every thread (%d errors)%s"
          % (late, label), late == 0)
    return mask, chunks, early, late


def tlbtest(q, cmd, timeout=180):
    """Runs tlbtest64 and hands back what arrived after the command.
    'runw' gives the prompt back at once and says nothing when the
    program ends, so what is waited for is the program's own verdict;
    'run' then holds the shell until it has reaped the program."""
    start = len(q.serial())
    q.type_line(cmd)
    deadline = time.time() + timeout
    while time.time() < deadline:
        out = q.serial()[start:]
        if "the memory agrees on every core" in out or "WRONG" in out:
            break
        time.sleep(0.3)
    if cmd.startswith("run "):
        end = time.time() + 30
        while "exit code" not in q.serial()[start:] and time.time() < end:
            time.sleep(0.3)
    return q.serial()[start:]


def wait_shell(q, timeout=90):
    """The first console's prompt. The other three consoles print theirs
    earlier, so the first 'thread64>' on the port does not mean the
    shell the keyboard talks to is there."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        out = q.serial()
        at = out.find("Commands:")
        if at >= 0 and "thread64>" in out[at:]:
            return out
        time.sleep(0.3)
    raise RuntimeError("timeout: the first console's shell never came up")


def main():
    print("--- changing a mapping and the other cores ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    # The page fault counters are the last line of the 'smp' report, so
    # waiting for them never reads half of the TLB line above
    last = "Page faults:"
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
            wait_shell(q)
            before = counters(q, last)
            check("the kernel counts mapping changes",
                  before is not None and before[0] > 0)

            # --- On the boot core: nobody else holds the space -------
            out = tlbtest(q, "run tlbtest64")
            res = run_case(q, out, " ('run')")
            mid = counters(q, last)
            if res:
                check("without 'runw' the process stays on the boot core "
                      "(mask %d)" % res[0], res[0] == 1)
            if before is None or mid is None:
                check("the TLB counters could be read after the attempt",
                      False)
                return summary()
            check("and it costs not one interrupt (%d -> %d)"
                  % (before[1], mid[1]), mid[1] == before[1] == 0)
            # ...though mappings did change - they were simply nobody
            # else's business
            check("even though there were many mapping changes "
                  "(%d -> %d local)" % (before[0], mid[0]),
                  mid[0] > before[0])

            # --- Allowed anywhere: threads of one process spread -----
            out = tlbtest(q, "runw tlbtest64")
            res = run_case(q, out, " ('runw')")
            after = counters(q, last)
            if after is None:
                check("the TLB counters could be read after the second "
                      "attempt", False)
                return summary()
            if res:
                mask = res[0]
                check("threads of one process would run off the "
                      "boot core (mask %d)" % mask, (mask & ~1) != 0)
                check("and more than one core carried them (%d)"
                      % bin(mask).count("1"), bin(mask).count("1") >= 2)
            check("the mapping changes had to reach the other cores "
                  "(%d -> %d)" % (mid[1], after[1]), after[1] > mid[1])
            check("and every one of them was answered (%d -> %d)"
                  % (mid[2], after[2]),
                  after[2] - mid[2] >= after[1] - mid[1])

            check("the system still runs after the attempt",
                  "[hello64] done, exit code 7" in
                  q.command("run hello64", "done, exit code", timeout=60))
        finally:
            q.close()

        print("--- the same attempt on one core ---")
        q1 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog1,
                  qemu="qemu-system-x86_64")
        try:
            wait_shell(q1)
            before = counters(q1, last)
            out = tlbtest(q1, "runw tlbtest64")
            res = run_case(q1, out, " (one core)")
            after = counters(q1, last)
            if before is None or after is None:
                check("the TLB counters could be read (one core)", False)
                return summary()
            if res:
                check("on one core 'runw' changes nothing (mask %d)"
                      % res[0], res[0] == 1)
            check("and there is nobody to send interrupts to (%d -> %d)"
                  % (before[1], after[1]), after[1] == before[1] == 0)
            check("and the mapping changes happened anyway (%d -> %d local)"
                  % (before[0], after[0]), after[0] > before[0])
        finally:
            q1.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, slog1):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
