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
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


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
        # that loses is taken again, up to four readings, and the best
        # ratio counts. The threshold does not move; what grew is the
        # patience, because four cores of a guest need four of the
        # host's, and the suite now shares the host with heavier
        # neighbours. A kernel that runs the threads one after another
        # comes out at four times the batch however often it is asked.
        for _ in range(3):
            if four < 3 * one or not again:
                break
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


def wait_shell(q, timeout=90):
    """The first console's prompt. The other three consoles print theirs
    earlier, so the first 'thread64>' on the port does not mean the
    shell the keyboard talks to is there - and a command waiting for the
    prompt after it would take one of theirs for its own."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        out = q.serial()
        at = out.find("Commands:")
        if at >= 0 and "thread64>" in out[at:]:
            return out
        time.sleep(0.3)
    raise RuntimeError("timeout: the first console's shell never came up")


def idle_per_core(ps, count):
    """Does every secondary core run its own idle task."""
    for i in range(1, count):
        if not re.search(r"idle%d" % i, ps):
            return False
    return True


def main():
    print("--- kernel threads on every core ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
    else:
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
                # The prompt comes after the whole answer, so a wait for
                # it never reads half of one
                ps = q.command("ps", "thread64>", timeout=30)
                check("every secondary core has an idle task of its own",
                      idle_per_core(ps, 4))

                out = q.command("smpthreads", "thread64>", timeout=180)
                check_threads(out, "", 4,
                              lambda: q.command("smpthreads", "thread64>",
                                                timeout=180))

                # The system is to work as before after all this
                after_run = q.command("run hello64", "exit code", timeout=60)
                check("the kernel still runs after the attempt",
                      "hello, I am a" in after_run)
            finally:
                q.close()

            # --- A single-core machine ---
            # The same code, one core: the threads are to compute the
            # same, only one after another
            print("--- the same attempt on one core ---")
            q1 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog1,
                      qemu="qemu-system-x86_64")
            try:
                wait_shell(q1)
                out = q1.command("smpthreads", "thread64>", timeout=180)
                check_threads(out, " (one core)", 1)
            finally:
                q1.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)
            for f in (iso, slog, slog1):
                if os.path.exists(f):
                    os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
