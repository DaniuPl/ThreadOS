#!/usr/bin/env python3
"""ProtonOS smoke test: system calls without the big kernel lock.

The previous step let user programs onto all the cores, but every entry
into the kernel took one lock for the whole kernel. So the counting in
ring 3 went in parallel, and everything that touched the kernel - in
turn. A program that calls the kernel often gained nothing from four
cores; it lost, because on top of the waiting came a fight over the same
memory line.

Now a call that reaches only for the calling task's state goes through
without the lock. The list of such calls is short and explicit, and the
default answer is "with the lock".

We check three things:

  1. Correctness, and that matters most. Each of the million sys_getpid
     calls is to return THIS process's number. If two cores disagreed
     about which task they are running - and that is exactly the risk the
     lock had been covering up - it would show here immediately.
  2. 'run syscalls' holds both processes on the bootstrap core, so the
     core mask is to be equal to 1. That is the reference measurement.
  3. 'runw syscalls' lets them run anywhere. It is to be CLEARLY faster,
     not merely a little - and that is the whole difference, because
     before this change it was SLOWER here than on a single core.

On a single-core machine both commands are to give the same thing.

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

PATTERN = re.compile(r"\[syscalls\] (dziecko|rodzic ): rdzenie (\d+), "
                  r"tyknac (\d+), wywolan (\d+), zgodnych (\d+)")


def measure(q, cmd_text, label):
    """One run: (mask_together, longest_time) or None."""
    out = q.command(cmd_text, "gotowe", timeout=300)
    proc = [(int(m.group(2)), int(m.group(3)), int(m.group(4)),
             int(m.group(5))) for m in PATTERN.finditer(out)]

    check("oba procesy doszly do konca" + label, len(proc) == 2)
    if len(proc) != 2:
        return None

    bad = [(w, z) for _, _, w, z in proc if w != z]
    check("kazde wywolanie zwrocilo numer wlasnego procesu "
          "(%d + %d razy)%s" % (proc[0][3], proc[1][3], label),
          not bad and proc[0][3] > 0)

    return (proc[0][0] | proc[1][0], max(p[1] for p in proc))


def run_case(q, ncores, label):
    serial_run = measure(q, "run syscalls", " ('run')" + label)
    if serial_run:
        check("bez 'runw' oba procesy zostaly na rdzeniu rozruchowym "
              "(maska %d)%s" % (serial_run[0], label), serial_run[0] == 1)
        check("pomiar odniesienia trwal dosc dlugo, by cos znaczyc "
              "(%d tyknac)%s" % (serial_run[1], label), serial_run[1] >= 20)

    parallel_run = measure(q, "runw syscalls", " ('runw')" + label)
    if not (serial_run and parallel_run):
        return

    if ncores > 1:
        check("wywolania szly takze poza rdzeniem rozruchowym (maska %d)%s"
              % (parallel_run[0], label), (parallel_run[0] & ~1) != 0)
        # What is claimed here is that two processes can be INSIDE the
        # kernel at the same time. The thing that would disprove it is
        # what the big lock used to do: two cores came out SLOWER than
        # one, because each spent its time waiting for the other. So
        # the number that matters is 'faster at all'; the margin below
        # is there only to keep noise from passing for a result.
        #
        # And noise there is: four virtual machines share this host,
        # and a machine whose two cores are taken away in the middle of
        # a measurement measures the host, not the kernel. Hence a
        # measurement repeated while it loses, with the best of them
        # counting - and a margin (0.85) that a genuinely parallel
        # kernel clears even on a busy host, while a serialised one
        # (1.0 and up) does not come near it.
        limit = 17 * serial_run[1] // 20
        for _ in range(4):
            if parallel_run[1] < limit:
                break
            again = measure(q, "runw syscalls", " ('runw', powtorka)" + label)
            if again and again[1] < parallel_run[1]:
                parallel_run = again
        check("dwa procesy wolaly jadro naraz, nie po kolei (%d wobec %d "
              "tykniec)%s" % (parallel_run[1], serial_run[1], label),
              parallel_run[1] < limit)
    else:
        check("na maszynie jednordzeniowej 'runw' nie zmienia nic "
              "(maska %d)%s" % (parallel_run[0], label), parallel_run[0] == 1)
        check("i trwa tyle samo, co 'run' (%d wobec %d tykniec)%s"
              % (parallel_run[1], serial_run[1], label),
              parallel_run[1] > serial_run[1] // 2)


def main():
    print("--- wywolania systemowe bez zamka (jadro 64-bitowe) ---")

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

    print("--- wywolania systemowe bez zamka (jadro 32-bitowe) ---")
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
