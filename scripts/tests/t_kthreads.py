#!/usr/bin/env python3
"""ProtonOS smoke test: kernel threads run by all the cores.

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

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def check_threads(out, suffix, ncores):
    check("proba doszla do konca" + suffix, "Watki poza" in out)
    if "Watki poza" not in out:
        return

    masks = [int(x, 16) for x in re.findall(r"watek\d: rdzenie ([0-9a-f]+)",
                                            out)]
    results = re.findall(r"watek\d: rdzenie [0-9a-f]+, wynik (\d+)", out)
    check("policzyly sie wszystkie cztery watki" + suffix,
          len(masks) == 4 and len(results) == 4)
    check("kazdy dal ten sam wynik, co porcja policzona w shellu"
          + suffix,
          "Wynik: zgadza sie" in out and len(set(results)) == 1)

    total = 0
    for m in masks:
        total |= m
    if ncores > 1:
        # The heart of it: a kernel thread really ran off the bootstrap core
        check("watki prowadzil takze rdzen inny niz rozruchowy (maska %x)%s"
              % (total, suffix),
              (total & ~1) != 0 and "Watki poza rdzeniem rozruchowym: tak"
              in out)
        core_count = bin(total).count("1")
        check("prowadzil je wiecej niz jeden rdzen (%d)%s"
              % (core_count, suffix), core_count >= 2)
    else:
        check("na jednym rdzeniu system mowi to wprost" + suffix,
              total == 1 and
              "Watki poza rdzeniem rozruchowym: NIE" in out)

    times = re.search(r"Jedna porcja: (\d+) tyknac, cztery naraz: (\d+)", out)
    check("czasy zmierzone" + suffix, times is not None)
    if not times:
        return
    one, four = int(times.group(1)), int(times.group(2))
    check("porcja odniesienia trwala dosc dlugo, by cos znaczyc (%d tyknac)%s"
          % (one, suffix), one >= 5)
    if ncores > 1:
        # One after another they would take four times as long. The
        # threshold stands halfway between one and the other - we are not
        # measuring the machine's speed, only whether the work goes in
        # parallel at all.
        check("cztery porcje naraz szybciej niz po kolei (%d wobec ~%d)%s"
              % (four, 4 * one, suffix),
              four < 3 * one)
    else:
        check("na jednym rdzeniu cztery porcje trwaja jak cztery (%d wobec"
              " ~%d)%s" % (four, 4 * one, suffix),
              four >= 2 * one)


def idle_per_core(ps, ile):
    """Does every secondary core run its own idle task."""
    for i in range(1, ile):
        if not re.search(r"bezczynny%d" % i, ps):
            return False
    return True


def main():
    print("--- watki jadra na wszystkich rdzeniach (jadro 64-bitowe) ---")

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
                ps = q.command("ps", "NAZWA", timeout=30)
                check("kazdy rdzen wtorny ma wlasne zadanie bezczynne",
                      idle_per_core(ps, 4))

                out = q.command("smpthreads", "Watki poza", timeout=180)
                check_threads(out, "", 4)

                # The system is to work as before after all this
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

    print("--- watki jadra na wszystkich rdzeniach (jadro 32-bitowe) ---")
    slog32 = tempfile.mktemp(suffix=".log")
    try:
        q3 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog32, cpus=4)
        try:
            q3.wait_for("ProtonOS shell", timeout=40)
            ps = q3.command("ps", "NAZWA", timeout=30)
            check("kazdy rdzen wtorny ma wlasne zadanie bezczynne (32-bit)",
                  idle_per_core(ps, 4))

            out = q3.command("smpthreads", "Watki poza", timeout=180)
            check_threads(out, " (32-bit)", 4)

            after_run = q3.command("run hello", "zakonczyl sie", timeout=60)
            check("jadro dziala dalej po probie (32-bit)",
                  "zakonczyl sie (kod 0)" in after_run)
        finally:
            q3.close()
    finally:
        if os.path.exists(slog32):
            os.unlink(slog32)

    # --- A single-core machine ---
    # The same code, one core: the threads are to compute the same, only
    # one after another
    print("--- ta sama proba na jednym rdzeniu ---")
    slog1 = tempfile.mktemp(suffix=".log")
    try:
        q1 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog1)
        try:
            q1.wait_for("ProtonOS shell", timeout=40)
            out = q1.command("smpthreads", "Watki poza", timeout=180)
            check_threads(out, " (jeden rdzen)", 1)
        finally:
            q1.close()
    finally:
        if os.path.exists(slog1):
            os.unlink(slog1)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
