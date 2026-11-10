#!/usr/bin/env python3
"""ThreadOS smoke test: user programs on all the cores.

Kernel threads ran on all the cores earlier already. A program in ring 3
is another matter, though: it needs its own task state segment on every
core, its own entry into the system calls, and the program's own page
tables loaded on whichever core picks it up. Kernel code gets by without
the last of those - the kernel's half of every address space is the
same - but a user program's address means nothing in tables that are
not its own, and the process dies.

We check four things:

  1. 'run cores' - two processes held on the bootstrap core. The core
     mask is to be equal to 1, that is, the mechanism does NOT spread
     things out by default.
  2. 'runw cores' - the same two processes, allowed to run anywhere. The
     mask is to have more than one bit, that is, ring 3 really was off
     the bootstrap core.
  3. Time. Both processes compute the same amount, so on one core each
     sees twice as long as on two. We compare 'run' with 'runw' in the
     same machine, so we are not measuring the host's speed.
  4. The checksum. A program that computed its share on another core is
     to give exactly the same number as one computed on the bootstrap
     core. Without that, points 2 and 3 would only say that something
     went faster.

On a single-core machine 'runw' is to work the same as 'run' - the
promise "allowed anywhere" is not a promise "must be somewhere else".

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64

PATTERN = re.compile(r"\[cores\] (child|parent ): cores (\d+), "
                  r"ticks (\d+), sum (\d+)")


def parse_lines(out):
    """[(mask, ticks, checksum), ...] for both processes."""
    return [(int(m.group(2)), int(m.group(3)), int(m.group(4)))
            for m in PATTERN.finditer(out)]


def check_pair(out, label):
    """Common to 'run' and 'runw': both processes reached the end and
    agree on the result. Returns (mask_together, most_ticks,
    checksum)."""
    proc = parse_lines(out)
    check("both processes ran to the end" + label, len(proc) == 2)
    if len(proc) != 2:
        return None

    sums = set(p[2] for p in proc)
    check("both counted exactly the same" + label, len(sums) == 1)

    mask = proc[0][0] | proc[1][0]
    ticks = max(p[1] for p in proc)
    return mask, ticks, sums.pop()


def run_case(q, ncores, label):
    """One machine: 'run' next to 'runw' and the comparison."""
    parallel_run = None

    serial_run = check_pair(
        q.command("run cores", "done", timeout=180), " ('run')" + label)
    if serial_run:
        mask, ticks, _ = serial_run
        check("without 'runw' both processes stayed on the boot core "
              "(mask %d)%s" % (mask, label), mask == 1)
        check("the reference batch lasted long enough to mean something "
              "(%d ticks)%s" % (ticks, label), ticks >= 20)

    parallel_run = check_pair(
        q.command("runw cores", "done", timeout=180),
        " ('runw')" + label)
    if not (serial_run and parallel_run):
        return

    check("the same checksum from 'run' and from 'runw'" + label,
          serial_run[2] == parallel_run[2])

    mask = parallel_run[0]
    if ncores > 1:
        check("a ring 3 program ran off the boot core "
              "too (mask %d)%s" % (mask, label),
              (mask & ~1) != 0)
        check("more than one core ran it (%d)%s"
              % (bin(mask).count("1"), label),
              bin(mask).count("1") >= 2)
        # Two processes on one core compute in turn, on two - at once. The
        # threshold stands halfway between one and the other.
        # A time measured once inside a virtual machine standing next to
        # three others is not evidence. When the first one loses, we
        # measure again - the threshold does not move, only the number
        # of readings it takes to believe one.
        # Up to four readings, and the best of them counts. The
        # threshold does not move; what grew is the patience, because
        # this machine now shares the host with three others running
        # heavier groups than they used to.
        for _ in range(3):
            if parallel_run[1] < 3 * serial_run[1] // 4:
                break
            again = check_pair(
                q.command("runw cores", "done", timeout=180),
                " ('runw', a repeat)" + label)
            if again and again[1] < parallel_run[1]:
                parallel_run = again
        check("two processes at once are faster than one after another (%d against %d "
              "ticks)%s" % (parallel_run[1], serial_run[1], label),
              parallel_run[1] < 3 * serial_run[1] // 4)
    else:
        check("on a single-core machine 'runw' changes nothing "
              "(mask %d)%s" % (mask, label), mask == 1)
        check("and it lasts as long as 'run' (%d against %d ticks)%s"
              % (parallel_run[1], serial_run[1], label),
              parallel_run[1] > serial_run[1] // 2)


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    print("--- processes on every core (four cores) ---")
    q = Qemu(KERNEL, INITRD, append=TEXT_MODE, cpus=4)
    try:
        q.wait_serial("thread64>", timeout=90)
        run_case(q, 4, "")
        # 'run' waits for its own child, so the shell's line about the
        # end is hello64's - with the code hello64 exits with
        after_run = q.command("run hello64", "[hello64] done, exit code",
                              timeout=60)
        check("the kernel still runs after the attempt",
              "hello, I am a" in after_run
              and "[hello64] done, exit code 7" in after_run)
    finally:
        q.close()

    print("--- the same attempt on one core ---")
    q1 = Qemu(KERNEL, INITRD, append=TEXT_MODE)
    try:
        q1.wait_serial("thread64>", timeout=90)
        run_case(q1, 1, " (one core)")
    finally:
        q1.close()

    return summary()


if __name__ == "__main__":
    sys.exit(main())
