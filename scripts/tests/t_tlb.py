#!/usr/bin/env python3
"""ProtonOS smoke test: telling the other cores that a mapping changed.

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
  4. It costs nothing where there is nobody to tell. 'run tlbtest'
     keeps the process on the boot core and must not send a single
     interrupt; on a single-core machine neither must 'runw'.

Both kernels, and the same four checks in each: long mode had no
threads at all until now, so the mechanism could be built into it but
not shown working.

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

HEAD = re.compile(r"\[tlbtest(?:64)?\] watkow (\d+), kawalkow (\d+) po (\d+) "
                  r"stron, maska rdzeni (\d+)")
BAD = re.compile(r"\[tlbtest(?:64)?\] bledow przy pisaniu (\d+), "
                 r"po wszystkim (\d+)")
COUNTERS = re.compile(r"TLB: lokalnie (\d+), zdalnie (\d+), odpowiedzi (\d+)")


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

    The screen holds twenty-five lines, and the kernel says something of
    its own about every thread that ends - so under a loaded host the
    report can scroll off the top before we read it. The serial log
    keeps everything, so when the screen has lost it we read that
    instead, taking the LAST report in it: that is the run we have just
    made."""
    h = HEAD.search(out)
    b = BAD.search(out)
    if not h or not b:
        log = q.serial()
        hits = list(HEAD.finditer(log))
        bits = list(BAD.finditer(log))
        h = hits[-1] if hits else None
        b = bits[-1] if bits else None
    check("tlbtest zdal raport%s" % label, bool(h) and bool(b))
    if not h or not b:
        return None
    threads, chunks, pages, mask = (int(x) for x in h.groups())
    early, late = (int(x) for x in b.groups())

    # A run that mapped nothing would pass every other check here
    check("kazdy watek dostal swoje kawalki (%d z %d)%s"
          % (chunks, threads * 10, label), chunks == threads * 10)
    check("pamiec zgodna przy pisaniu (%d bledow)%s" % (early, label),
          early == 0)
    # The one that matters: read after every thread has finished, so
    # after the neighbours had time to be given somebody else's frames
    check("pamiec zgodna po wszystkich watkach (%d bledow)%s"
          % (late, label), late == 0)
    return mask, chunks, early, late


def main():
    print("--- zmiana mapowania a inne rdzenie (jadro 32-bitowe) ---")
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog, cpus=4)
        try:
            q.wait_for("ProtonOS shell", timeout=40)

            before = counters(q, "TLB:")
            check("jadro liczy zmiany mapowan", before is not None)

            # --- On the boot core: nobody else holds the space -------
            out = q.command("run tlbtest", "zakonczyl sie", timeout=180)
            res = run_case(q, out, " ('run')")
            mid = counters(q, "TLB:")
            if res:
                check("bez 'runw' proces zostaje na rdzeniu rozruchowym "
                      "(maska %d)" % res[0], res[0] == 1)
            if before is None or mid is None:
                check("liczniki TLB daly sie odczytac po probie", False)
                return summary()
            check("i nie kosztuje ani jednego przerwania (%d -> %d)"
                  % (before[1], mid[1]), mid[1] == before[1])
            # ...though mappings did change - they were simply nobody
            # else's business
            check("choc zmian mapowan bylo duzo (%d -> %d lokalnych)"
                  % (before[0], mid[0]), mid[0] > before[0])

            # --- Allowed anywhere: threads of one process spread -----
            out = q.command("runw tlbtest", "zakonczyl sie", timeout=180)
            res = run_case(q, out, " ('runw')")
            after = counters(q, "TLB:")
            if after is None:
                check("liczniki TLB daly sie odczytac po drugiej probie",
                      False)
                return summary()
            if res:
                mask = res[0]
                check("watki jednego procesu bieglyby poza rdzeniem "
                      "rozruchowym (maska %d)" % mask, (mask & ~1) != 0)
                check("i niosl je wiecej niz jeden rdzen (%d)"
                      % bin(mask).count("1"), bin(mask).count("1") >= 2)
            check("zmiany mapowan musialy dojsc do innych rdzeni "
                  "(%d -> %d)" % (mid[1], after[1]), after[1] > mid[1])
            check("i kazda z nich doczekala sie odpowiedzi (%d -> %d)"
                  % (mid[2], after[2]), after[2] - mid[2] >= after[1] - mid[1])

            check("system dziala dalej po probie",
                  "zakonczyl sie (kod 0)" in
                  q.command("run hello", "zakonczyl sie", timeout=60))
        finally:
            q.close()
    finally:
        if os.path.exists(slog):
            os.unlink(slog)

    print("--- ta sama proba na jednym rdzeniu ---")
    slog1 = tempfile.mktemp(suffix=".log")
    try:
        q1 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog1)
        try:
            q1.wait_for("ProtonOS shell", timeout=40)
            before = counters(q1, "TLB:")
            out = q1.command("runw tlbtest", "zakonczyl sie", timeout=180)
            res = run_case(q1, out, " (jeden rdzen)")
            after = counters(q1, "TLB:")
            if before is None or after is None:
                check("liczniki TLB daly sie odczytac (jeden rdzen)", False)
                return summary()
            if res:
                check("na jednym rdzeniu 'runw' nie zmienia nic (maska %d)"
                      % res[0], res[0] == 1)
            check("i nie ma komu wysylac przerwan (%d -> %d)"
                  % (before[1], after[1]), after[1] == before[1] == 0)
            check("a zmiany mapowan i tak byly (%d -> %d lokalnych)"
                  % (before[0], after[0]), after[0] > before[0])
        finally:
            q1.close()
    finally:
        if os.path.exists(slog1):
            os.unlink(slog1)

    # The same thing in long mode. Until now there were no threads
    # there at all, so two cores could not be put into one address
    # space and the mechanism could be built in but not shown working.
    print("--- to samo w jadrze 64-bitowym ---")
    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog64 = tempfile.mktemp(suffix=".log")
    try:
        check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog64, cpus=4,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=90)
            before = counters(q, "TLB:")
            check("jadro 64-bitowe tez liczy zmiany mapowan",
                  before is not None and before[0] > 0)

            out = q.command("run tlbtest64", "kod wyjscia", timeout=180)
            res = run_case(q, out, " (64-bit, 'run')")
            mid = counters(q, "TLB:")
            if res:
                check("bez 'runw' proces zostaje na rdzeniu rozruchowym "
                      "(64-bit, maska %d)" % res[0], res[0] == 1)
            if before is None or mid is None:
                check("liczniki TLB daly sie odczytac (64-bit)", False)
                return summary()
            check("i nie kosztuje ani jednego przerwania (64-bit, %d -> %d)"
                  % (before[1], mid[1]), mid[1] == before[1] == 0)

            out = q.command("runw tlbtest64", "kod wyjscia", timeout=180)
            res = run_case(q, out, " (64-bit, 'runw')")
            after = counters(q, "TLB:")
            if after is None:
                check("liczniki TLB daly sie odczytac po drugiej probie "
                      "(64-bit)", False)
                return summary()
            if res:
                mask = res[0]
                check("watki jednego procesu bieglyby poza rdzeniem "
                      "rozruchowym (64-bit, maska %d)" % mask,
                      (mask & ~1) != 0)
                check("i niosl je wiecej niz jeden rdzen (64-bit, %d)"
                      % bin(mask).count("1"), bin(mask).count("1") >= 2)
            check("zmiany mapowan musialy dojsc do innych rdzeni "
                  "(64-bit, %d -> %d)" % (mid[1], after[1]),
                  after[1] > mid[1])
            check("i kazda z nich doczekala sie odpowiedzi (64-bit, %d -> %d)"
                  % (mid[2], after[2]),
                  after[2] - mid[2] >= after[1] - mid[1])

            check("jadro dziala dalej po probie (64-bit)",
                  "czesc, jestem ELF-em" in
                  q.command("run hello64", "kod wyjscia", timeout=60))
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog64):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
