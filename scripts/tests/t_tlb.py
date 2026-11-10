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
from ptest import check, summary, make_iso64, no64, _kernel_define

THREADS = _kernel_define("user64/tlbtest64.c", "THREADS")
ROUNDS = _kernel_define("user64/tlbtest64.c", "ROUNDS")

HEAD = re.compile(r"\[tlbtest64\] threads (\d+), chunks (\d+) of (\d+) "
                  r"pages, core mask (\d+)")
BAD = re.compile(r"\[tlbtest64\] errors while writing (\d+), "
                 r"afterwards (\d+)")
# The line counts only when it has arrived whole, or a number cut in half
# reads as a different number
COUNTERS = re.compile(r"TLB: local (\d+), remote (\d+), replies (\d+)\r?\n")
# tlbtest64's last line, whichever way it went
VERDICTS = ("[tlbtest64] the memory agrees on every core",
            "[tlbtest64] WRONG",
            "[tlbtest64] the thread could not be created")
# The shell's line about the end of a program 'run' waited for
DONE = re.compile(r"\[tlbtest64\] done, exit code (-?\d+) \(")
# hello64's last words, and after them the shell's line about its end
HELLO_END = re.compile(r"\[hello64\] ending politely\r?\n[\s\S]*?"
                       r"\[hello64\] done, exit code (-?\d+) \(")


def answer(q, cmd, until, timeout):
    """Types a command and reads what arrived after it until until(text)
    says the answer is all there, or the time runs out."""
    start = len(q.serial())
    q.type_line(cmd)
    deadline = time.time() + timeout
    out = ""
    while time.time() < deadline:
        out = q.serial()[start:]
        if until(out):
            break
        time.sleep(0.3)
    return out


def launch(q, cmd, timeout=180):
    """Starts tlbtest64 and returns what arrived after the command, up to
    the end of the run.

    'runw' does not wait for the program, so nothing but the program
    itself says when it is over: its verdict, the good one or the bad
    one, is what is waited for. 'run' waits, and its shell reports the
    end with the program's exit code - which then has to be there too."""
    if cmd.startswith("runw "):
        return answer(q, cmd, lambda o: any(v in o for v in VERDICTS),
                      timeout)
    return answer(q, cmd, DONE.search, timeout)


def counters(q, timeout=60):
    """(local only, sent to other cores, answers collected).

    Asked more than once on purpose: the counters are read with an
    ordinary shell command, and on a host running the whole suite at
    once the answer sometimes does not arrive inside the first
    timeout. A test that then dies of a missing value teaches nothing
    about the kernel."""
    for _ in range(3):
        m = COUNTERS.search(answer(q, "smp", COUNTERS.search, timeout))
        if m:
            return tuple(int(x) for x in m.groups())
        time.sleep(1.0)
    return None


def run_case(out, label):
    """What the program itself reports: (mask, chunks, early, late).

    Only what arrived after the command counts: the report of an
    earlier run is in the log too, and would answer for this one."""
    h = HEAD.search(out)
    b = BAD.search(out)
    check("tlbtest64 reported back%s" % label, bool(h) and bool(b))
    if not h or not b:
        return None
    _, chunks, pages, mask = (int(x) for x in h.groups())
    early, late = (int(x) for x in b.groups())

    # A run that mapped nothing would pass every other check here. The
    # number wanted comes from the program's source, not from the
    # thread count it prints - that one is a constant it cannot get
    # wrong.
    want = THREADS * ROUNDS
    check("every thread got its chunks (%d of %d)%s"
          % (chunks, want, label), chunks == want)
    check("the memory agrees while writing (%d errors)%s" % (early, label),
          early == 0)
    # The one that matters: read after every thread has finished, so
    # after the neighbours had time to be given somebody else's frames
    check("the memory agrees after every thread (%d errors)%s"
          % (late, label), late == 0)
    return mask, chunks, early, late


def main():
    print("--- changing a mapping and the other cores ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    slog1 = tempfile.mktemp(suffix=".log")
    try:
        check("the disc with the kernel builds", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, cpus=4)
        try:
            q.wait_serial("thread64>", timeout=90)
            before = counters(q)
            check("the kernel counts mapping changes",
                  before is not None and before[0] > 0)

            # --- On the boot core: nobody else holds the space -------
            out = launch(q, "run tlbtest64")
            res = run_case(out, " ('run')")
            done = DONE.search(out)
            check("'run' waited for the program and got its exit code "
                  "(%s)" % (done.group(1) if done else "none"),
                  done is not None and done.group(1) == "0")
            mid = counters(q)
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
            out = launch(q, "runw tlbtest64")
            res = run_case(out, " ('runw')")
            after = counters(q)
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

            # And a program after it runs to its end: hello64 ends with
            # 7, and the shell's word about it has to come after its own
            # last line and carry that 7 - the 'runw' program before it
            # has ended too, and its code is not this one's
            out = answer(q, "run hello64", HELLO_END.search, timeout=60)
            end = HELLO_END.search(out)
            check("the kernel still runs after the attempt (%s)"
                  % ("exit code " + end.group(1) if end else
                     "no end reported after the program's last line"),
                  "[hello64] hello, I am a" in out
                  and end is not None and end.group(1) == "7")
        finally:
            q.close()

        print("--- the same attempt on one core ---")
        q1 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog1)
        try:
            q1.wait_serial("thread64>", timeout=90)
            before = counters(q1)
            out = launch(q1, "runw tlbtest64")
            res = run_case(out, " (one core)")
            after = counters(q1)
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
