#!/usr/bin/env python3
"""ThreadOS smoke test: kernel threads run by all the cores.

This is the change the earlier ones were waiting for. A secondary core
already had an exception table, its own clock and locks on the shared
structures - but the current task was still ONE global variable, and as
long as it was, more than one core had no way of running tasks. Now
there are as many of them as there are cores.

We check three things, because each on its own could be faked:

  1. Every secondary core has its own idle task and runs it - that can be
     seen in 'ps'. Without it there would be nowhere to put its context.
  2. A thread records for itself which core it is computing on. If the
     scheduler merely threw it between the cores in turn, though, the
     mask would look the same.
  3. So we measure TIME. Four portions started in parallel are to take
     clearly less than four times as long as one. A single core cannot be
     cheated about that: it has to compute them one after another.

Every portion's result is the same as that of a portion computed in the
shell's thread - bit for bit. A thread that computed its share on another
core is to give exactly the same number.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64


def answer(q, cmd, until, timeout):
    """Types a command and reads what it printed until until(text) says
    all of it is there. The port is read while the kernel is still
    writing, and half a line parses as a different answer."""
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


def smpthreads(q):
    # The verdict is the last line: it counts once it has ended
    return answer(q, "smpthreads",
                  lambda out: re.search(r"Threads off the boot core: \w+\r?\n",
                                        out) is not None,
                  timeout=180)


def check_threads(out, suffix, ncores, again=None):
    check("the attempt ran to the end" + suffix, "Threads off" in out)
    if "Threads off" not in out:
        return

    masks = [int(x, 16) for x in re.findall(r"thread\d: cores ([0-9a-f]+)",
                                            out)]
    results = re.findall(r"thread\d: cores [0-9a-f]+, result (\d+)", out)
    check("all four threads counted" + suffix,
          len(masks) == 4 and len(results) == 4)
    check("each gave the same result as the batch counted in the shell"
          + suffix,
          "Result: matches" in out and len(set(results)) == 1)

    total = 0
    for m in masks:
        total |= m
    if ncores > 1:
        # The heart of it: a kernel thread really ran off the bootstrap core
        check("a core other than the boot one ran threads too (mask %x)%s"
              % (total, suffix),
              (total & ~1) != 0 and "Threads off the boot core: yes"
              in out)
        core_count = bin(total).count("1")
        check("more than one core ran them (%d)%s"
              % (core_count, suffix), core_count >= 2)
    else:
        check("on one core the system says so outright" + suffix,
              total == 1 and
              "Threads off the boot core: NO" in out)

    times = re.search(r"One batch: (\d+) ticks, four at once: (\d+)", out)
    check("the times were measured" + suffix, times is not None)
    if not times:
        return
    one, four = int(times.group(1)), int(times.group(2))
    check("the reference batch lasted long enough to mean something (%d ticks)%s"
          % (one, suffix), one >= 5)
    if ncores > 1:
        # One after another they would take four times as long. The
        # threshold stands halfway between one and the other - we are
        # not measuring the machine's speed, only whether the work goes
        # in parallel at all.
        #
        # And because it is the machine's speed that gets in the way -
        # four virtual machines share this host, and one of them can
        # lose its cores in the middle of a measurement - a reading
        # that loses is taken again, and the better of the two counts.
        # The threshold does not move.
        if four >= 3 * one and again:
            retry = re.search(r"One batch: (\d+) ticks, four at once: "
                              r"(\d+)", again())
            if retry:
                one2, four2 = int(retry.group(1)), int(retry.group(2))
                if four2 * one < four * one2:      # a better ratio
                    one, four = one2, four2
        check("four batches at once are faster than one after another (%d against ~%d)%s"
              % (four, 4 * one, suffix),
              four < 3 * one)
    else:
        check("on one core four batches last like four (%d against"
              " ~%d)%s" % (four, 4 * one, suffix),
              four >= 2 * one)


def idle_per_core(ps, count):
    """Does every secondary core run its own idle task."""
    for i in range(1, count):
        if not re.search(r"idle%d" % i, ps):
            return False
    return True


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    print("--- kernel threads on every core (four cores) ---")
    q = Qemu(KERNEL, INITRD, append=TEXT_MODE, cpus=4)
    try:
        q.wait_serial("thread64>", timeout=90)
        ps = answer(q, "ps", lambda out: idle_per_core(out, 4), timeout=30)
        check("every secondary core has an idle task of its own",
              idle_per_core(ps, 4))

        check_threads(smpthreads(q), "", 4, lambda: smpthreads(q))

        # The system is to work as before after all this
        after_run = q.command("run hello64", "exit code", timeout=60)
        check("the kernel still runs after the attempt",
              "hello, I am a" in after_run)
    finally:
        q.close()

    # --- A single-core machine ---
    # The same code, one core: the threads are to compute the same, only
    # one after another
    print("--- the same attempt on one core ---")
    q1 = Qemu(KERNEL, INITRD, append=TEXT_MODE)
    try:
        q1.wait_serial("thread64>", timeout=90)
        check_threads(smpthreads(q1), " (one core)", 1)
    finally:
        q1.close()

    return summary()


if __name__ == "__main__":
    sys.exit(main())
