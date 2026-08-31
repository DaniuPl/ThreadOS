#!/usr/bin/env python3
"""ProtonOS smoke test: user programs on all the cores.

Kernel threads ran on all the cores earlier already. A program in ring 3
is another matter, though: it needs its own task state segment on every
core, its own entry into the system calls - and, as turned out to matter
most, PAGING ENABLED. A secondary core in protected mode started without
it: for kernel code that came to the same thing, because low memory is
mapped one to one, but a user program's address meant nothing without
page tables and the process died.

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
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64

PATTERN = re.compile(r"\[cores\] (dziecko|rodzic ): rdzenie (\d+), "
                  r"tyknac (\d+), suma (\d+)")


def parse_lines(out):
    """[(mask, ticks, checksum), ...] for both processes."""
    return [(int(m.group(2)), int(m.group(3)), int(m.group(4)))
            for m in PATTERN.finditer(out)]


def check_pair(out, label):
    """Common to 'run' and 'runw': both processes reached the end and
    agree on the result. Returns (mask_together, most_ticks,
    checksum)."""
    proc = parse_lines(out)
    check("oba procesy doszly do konca" + label, len(proc) == 2)
    if len(proc) != 2:
        return None

    sums = set(p[2] for p in proc)
    check("oba policzyly dokladnie to samo" + label, len(sums) == 1)

    mask = proc[0][0] | proc[1][0]
    ticks = max(p[1] for p in proc)
    return mask, ticks, sums.pop()


def run_case(q, ncores, label):
    """One machine: 'run' next to 'runw' and the comparison."""
    parallel_run = None

    serial_run = check_pair(
        q.command("run cores", "gotowe", timeout=180), " ('run')" + label)
    if serial_run:
        mask, ticks, _ = serial_run
        check("bez 'runw' oba procesy zostaly na rdzeniu rozruchowym "
              "(maska %d)%s" % (mask, label), mask == 1)
        check("porcja odniesienia trwala dosc dlugo, by cos znaczyc "
              "(%d tyknac)%s" % (ticks, label), ticks >= 20)

    parallel_run = check_pair(
        q.command("runw cores", "gotowe", timeout=180),
        " ('runw')" + label)
    if not (serial_run and parallel_run):
        return

    check("ta sama suma kontrolna z 'run' i z 'runw'" + label,
          serial_run[2] == parallel_run[2])

    mask = parallel_run[0]
    if ncores > 1:
        check("program w pierscieniu 3 biegl takze poza rdzeniem "
              "rozruchowym (maska %d)%s" % (mask, label),
              (mask & ~1) != 0)
        check("prowadzil go wiecej niz jeden rdzen (%d)%s"
              % (bin(mask).count("1"), label),
              bin(mask).count("1") >= 2)
        # Two processes on one core compute in turn, on two - at once. The
        # threshold stands halfway between one and the other.
        check("dwa procesy naraz szybciej niz po kolei (%d wobec %d "
              "tykniec)%s" % (parallel_run[1], serial_run[1], label),
              parallel_run[1] < 3 * serial_run[1] // 4)
    else:
        check("na jednej maszynie jednordzeniowej 'runw' nie zmienia nic "
              "(maska %d)%s" % (mask, label), mask == 1)
        check("i trwa tyle samo, co 'run' (%d wobec %d tykniec)%s"
              % (parallel_run[1], serial_run[1], label),
              parallel_run[1] > serial_run[1] // 2)


def main():
    print("--- procesy na wszystkich rdzeniach (jadro 64-bitowe) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
    else:
        d = tempfile.mkdtemp()
        iso = tempfile.mktemp(suffix=".iso")
        slog = tempfile.mktemp(suffix=".log")
        try:
            check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
            if not os.path.exists(iso):
                return summary()

            q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, cpus=4,
                     qemu="qemu-system-x86_64")
            try:
                q.wait_serial("proton64>", timeout=90)
                run_case(q, 4, "")
                after_run = q.command("run hello64", "kod wyjscia", timeout=60)
                check("jadro dziala dalej po probie",
                      "czesc, jestem ELF-em" in after_run)
            finally:
                q.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)
            for f in (iso, slog):
                if os.path.exists(f):
                    os.unlink(f)

    print("--- procesy na wszystkich rdzeniach (jadro 32-bitowe) ---")
    slog32 = tempfile.mktemp(suffix=".log")
    try:
        q3 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog32, cpus=4)
        try:
            q3.wait_for("ProtonOS shell", timeout=40)
            run_case(q3, 4, " (32-bit)")
            after_run = q3.command("run hello", "zakonczyl sie", timeout=60)
            check("jadro dziala dalej po probie (32-bit)",
                  "zakonczyl sie (kod 0)" in after_run)
        finally:
            q3.close()
    finally:
        if os.path.exists(slog32):
            os.unlink(slog32)

    print("--- ta sama proba na jednym rdzeniu ---")
    slog1 = tempfile.mktemp(suffix=".log")
    try:
        q1 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog1)
        try:
            q1.wait_for("ProtonOS shell", timeout=40)
            run_case(q1, 1, " (jeden rdzen)")
        finally:
            q1.close()
    finally:
        if os.path.exists(slog1):
            os.unlink(slog1)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
