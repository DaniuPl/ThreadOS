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
served before the lock is taken (kernel64/isr64.c). The list is short
and explicit, like the list of system calls that need no lock, and the
default answer is still "with the lock".

What is asserted here is the mechanism, not a stopwatch. The kernel
counts the faults it served each way, and that count says outright what
a timing measurement can only suggest: the number served WITHOUT the
lock grows by at least the number of pages touched, and the number that
needed it does not move at all. Two programs ask it, each of a path of
its own:

  faults     two processes (a fork, so copy-on-write as well), each in
             its own address space, touching thousands of fresh HEAP
             pages;
  tlbtest64  four threads of ONE process, each asking for fresh
             anonymous memory in the mapping area round after round.

That holds with the program kept on the boot core ('run'), allowed onto
every core ('runw'), and on a machine with a single core. On several
cores at once the count is one locked addition per fault, and not one
may go missing - so there the bound has no slack at all.

Correctness is asserted just as hard, and it is the reason the counters
are worth anything. faults sums what it wrote and the last byte of
every fresh page, which has to be zero: the sum is worked out here as
well, and a frame handed to two processes at once - or a page that came
with somebody else's data - gives a different one. tlbtest64 checks
every chunk twice, and a frame handed to two threads at once shows up as
a wrong pattern.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, _kernel_define

# What the programs are built with - read from their sources, so that a
# changed constant cannot leave the test counting something else
PAGES = _kernel_define("user/faults.c", "PAGES")
THREADS = _kernel_define("user64/tlbtest64.c", "THREADS")
ROUNDS = _kernel_define("user64/tlbtest64.c", "ROUNDS")

# The line ends the 'smp' output; it counts only when it has arrived
# whole, or a number cut in half reads as a different number
COUNTERS = re.compile(r"Page faults: without the lock (\d+), with it (\d+)\r?\n")

ROW = re.compile(r"\[faults\] (child|parent ): cores (\d+), ticks (\d+), "
                 r"pages (\d+), sum (\d+)\r?\n")
# The parent's last line, after the child's report and its own
FAULTS_END = re.compile(r"\[faults\] done\r?\n")
FAULTS_RAN = re.compile(r"\[faults\] done, exit code (-?\d+) \(")

REPORT = re.compile(r"\[tlbtest64\] threads (\d+), chunks (\d+) of (\d+) "
                    r"pages, core mask (\d+)")
PASSED = "[tlbtest64] the memory agrees on every core"
FAILED = "[tlbtest64] WRONG"
TLB_RAN = re.compile(r"\[tlbtest64\] done, exit code (-?\d+) \(")

# hello64's last words, and after them the shell's line about its end
HELLO_END = re.compile(r"\[hello64\] ending politely\r?\n[\s\S]*?"
                       r"\[hello64\] done, exit code (-?\d+) \(")


def faults_sum(pages):
    """What faults has to count: per page, the two bytes it wrote and the
    last byte, which nobody wrote and so has to be a zero."""
    return sum((i & 0xFF) + ((i * 7 + 3) & 0xFF)
               for i in range(pages)) & 0xFFFFFFFF


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


def faults(q, timeout=60):
    """(served without the big lock, served with it)."""
    m = COUNTERS.search(answer(q, "smp", COUNTERS.search, timeout))
    return tuple(int(x) for x in m.groups()) if m else None


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


def ran(out, done, label):
    """'run' waits for its program and reports the end - with that
    program's code, after the program has said everything it had to."""
    m = done.search(out)
    check("'run' waited for the program and got its exit code (%s)%s"
          % (m.group(1) if m else "none", label),
          m is not None and m.group(1) == "0")


def faults_case(q, cmd, label, timeout=300):
    """Runs faults and waits for its end: with 'run' the shell's word
    about it, with 'runw' - which waits for nothing and so says nothing -
    the program's own last line. Returns (core mask of both processes,
    fresh pages touched by both, pages copied after the fork) or None."""
    until = FAULTS_END.search if cmd.startswith("runw ") else FAULTS_RAN.search
    out = answer(q, cmd, until, timeout)
    rows = ROW.findall(out)
    check("both processes reported back%s (%d of 2)" % (label, len(rows)),
          len(rows) == 2 and {r[0] for r in rows} == {"child", "parent "})
    if not cmd.startswith("runw "):
        ran(out, FAULTS_RAN, label)
    if len(rows) != 2:
        return None
    mask = 0
    for r in rows:
        mask |= int(r[1])
    sums = [int(r[4]) for r in rows]
    # Two processes doing the same work have to get the same answer, and
    # it is known in advance - a frame handed to both, or one that came
    # with somebody else's bytes in it, would not give it. The sum is
    # also what says that all PAGES pages were touched: the page count
    # in the row is the program's constant, printed as it stands, and
    # comparing with it would prove nothing.
    want = faults_sum(PAGES)
    check("both counted exactly what was written over fresh pages "
          "(%s, wanted %d)%s" % (", ".join(map(str, sums)), want, label),
          sums == [want, want])
    # Besides its fresh heap pages each process writes to the stack it
    # had at the fork - the call into touch() alone does that - and that
    # page is shared until then: one copy-on-write fault each, served
    # without the lock as well.
    return mask, 2 * PAGES, 2


def tlb_case(q, cmd, label, timeout=240):
    """Runs tlbtest64 and waits for its verdict, the good one or the bad
    one (and with 'run' for the shell's word about its end too).
    Returns (core mask, pages touched) or None."""
    if cmd.startswith("runw "):
        until = lambda o: PASSED in o or FAILED in o
    else:
        until = TLB_RAN.search
    out = answer(q, cmd, until, timeout)
    check("the program passed%s" % label,
          PASSED in out and FAILED not in out)
    if not cmd.startswith("runw "):
        ran(out, TLB_RAN, label)
    m = REPORT.search(out)
    check("the program reported what it touched%s" % label, m is not None)
    if not m:
        return None
    _, chunks, per_chunk, mask = (int(x) for x in m.groups())
    # The count wanted comes from the program's source: the thread count
    # it prints is a constant, and comparing with it proves nothing
    check("every thread got all its chunks (%d of %d)%s"
          % (chunks, THREADS * ROUNDS, label), chunks == THREADS * ROUNDS)
    return mask, chunks * per_chunk


def lockless(before, after, pages, label, copied=0):
    """Every fresh page went the fast way, and nothing took the lock.

    'copied' is how many copy-on-write faults the run is known to make
    on top of its fresh pages. Without them the bound would forgive as
    many lost counts as there are such faults - and a count lost by two
    cores adding at once is exactly what it is there to catch."""
    if before is None or after is None:
        check("the fault counters could be read%s" % label, False)
        return
    # At least one fault per fresh page: each of them is new in a space
    # nobody had touched. More is fine - stacks fault the same way - but
    # fewer would mean some went elsewhere, or that two cores counting
    # at once lost one.
    grew = after[0] - before[0]
    check("faults served without the lock (%d, pages %d%s)%s"
          % (grew, pages, " and %d copied" % copied if copied else "",
             label), grew >= pages + copied)
    check("and not one had to take it (%d)%s"
          % (after[1] - before[1], label), after[1] == before[1])


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    print("--- page faults without the big lock (four cores) ---")
    q = Qemu(KERNEL, INITRD, append=TEXT_MODE, cpus=4)
    try:
        q.wait_serial("thread64>", timeout=90)
        before = faults(q)
        check("the kernel counts page faults", before is not None)

        # --- Two processes, held on the boot core -----------------
        res = faults_case(q, "run faults", " (faults, 'run')")
        mid = settled(q, faults)
        if res:
            mask, pages, copied = res
            check("without 'runw' both stay on the boot core "
                  "(mask %d)" % mask, mask == 1)
            lockless(before, mid, pages, " (faults, 'run')", copied)

        # --- Allowed anywhere -------------------------------------
        # Here the two fault on several cores at once, and the count
        # is one locked addition each: a plain ++ lost a few dozen of
        # eighteen thousand now and then, which a bound with any slack
        # would hide.
        res = faults_case(q, "runw faults", " (faults, 'runw')")
        mid2 = settled(q, faults)
        if res:
            mask, pages, copied = res
            check("with 'runw' they ran off the boot core too "
                  "(mask %d)" % mask, (mask & ~1) != 0)
            lockless(mid, mid2, pages, " (faults, 'runw')", copied)

        # --- Threads of one process, in the mapping area ----------
        res = tlb_case(q, "run tlbtest64", " (tlbtest64, 'run')")
        mid3 = settled(q, faults)
        if res:
            mask, pages = res
            check("without 'runw' the threads stay on the boot core "
                  "(mask %d)" % mask, mask == 1)
            lockless(mid2, mid3, pages, " (tlbtest64, 'run')")

        res = tlb_case(q, "runw tlbtest64", " (tlbtest64, 'runw')")
        after = settled(q, faults)
        if res:
            mask, pages = res
            check("with 'runw' the threads ran off the boot core too "
                  "(mask %d)" % mask, (mask & ~1) != 0)
            lockless(mid3, after, pages, " (tlbtest64, 'runw')")

        # The shell's verdict on a program comes after the program's
        # own last line, and carries that program's own code: hello64
        # ends with 7. The 'runw' programs before it have ended too, and
        # their codes are not this one's.
        out = answer(q, "run hello64", HELLO_END.search, timeout=60)
        check("the system still runs programs after the attempt",
              "[hello64] ending politely" in out)
        done = HELLO_END.search(out)
        early = re.search(r"\[hello64\] done, exit code (-?\d+) \(", out)
        check("and 'run' reports that program's own exit code (%s)"
              % (done.group(1) if done else
                 "%s, before the program had ended" % early.group(1)
                 if early else "none"),
              done is not None and done.group(1) == "7")
    finally:
        q.close()

    print("--- the same attempt on one core ---")
    q1 = Qemu(KERNEL, INITRD, append=TEXT_MODE)
    try:
        q1.wait_serial("thread64>", timeout=90)
        before = faults(q1)
        res = faults_case(q1, "runw faults", " (faults, one core)")
        mid = settled(q1, faults)
        if res:
            mask, pages, copied = res
            check("on one core the mask is 1 (%d)" % mask, mask == 1)
            lockless(before, mid, pages, " (faults, one core)", copied)
        res = tlb_case(q1, "runw tlbtest64", " (tlbtest64, one core)")
        after = settled(q1, faults)
        if res:
            mask, pages = res
            check("on one core the threads' mask is 1 (%d)" % mask,
                  mask == 1)
            lockless(mid, after, pages, " (tlbtest64, one core)")
    finally:
        q1.close()

    return summary()


sys.exit(main())
