#!/usr/bin/env python3
"""ThreadOS smoke test: detecting and waking the remaining processors.

After power on one core works - the one the firmware chose as the
bootstrap. The rest stand and wait for a signal, and that is the
hardware's decision, not the system's: a processor woken by a SIPI
signal starts in real mode, sixteen bits wide, at the address
(vector << 12) - exactly the way a PC started forty years ago.

This is the first step, not the whole of SMP. A woken core walks its own
road to protected mode, reports in and stands by. Giving it work needs a
per-core scheduler and locks in every kernel structure, and those were
written for a single core and quietly count on it.

The secondary cores do not run tasks of their own - the scheduler and
the process list were written for a single core. What they do get is
countable work: a shared queue of items, from which each takes the next
number under a lock. That is all the state shared between the cores -
one number and one lock.

The right question to ask about concurrency is not "did anything get
computed", but "was every item computed EXACTLY ONCE". That is why we
compare the result of the distributed work with the one computed on a
single core: it matches only when no item was lost or counted twice, and
nobody trampled somebody else's cell.

The test also checks that the system works exactly the same after all
this. "I see four processors" by itself would be worth nothing if
something in the system broke along the way.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def values(text, label):
    """The numbers from ONE line of the 'smp' output.

    The command now gives two lines of the same shape - the share of the
    work and the ticks of the cores' own clocks - so looking for
    'rdzenN=' across the whole text would glue one to the other."""
    for w in text.replace("\r", "").split("\n"):
        if w.startswith(label):
            return [int(x) for x in re.findall(r"core\d+=(\d+)", w)]
    return []


def main():
    print("--- the other processors (INIT-SIPI) ---")

    # --- Four cores ---
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog, cpus=4)
    try:
        start = q.wait_for("Processors:", timeout=60)
        m = re.search(r"Processors: (\d+) \(woken (\d+)", start)
        check("the system lists every processor from the ACPI table",
              m is not None and int(m.group(1)) == 4)
        check("the other three cores answered after INIT-SIPI",
              m is not None and int(m.group(2)) == 3)

        q.wait_for("ThreadOS shell", timeout=30)
        q.wait_for("[net] DHCP", timeout=30)

        # After waking the cores the system is to work as it did before
        up1 = q.command("uptime", "timer ticks")
        check("the timer ticks once the other cores are woken",
              "timer ticks" in up1)

        proc = q.command("run hello", "has ended", timeout=40)
        check("a ring 3 process runs as before",
              "[hello] hello, I am an ELF process" in proc)

        files = q.command("write smptest multicore", "wrote")
        check("the disk works as before", "wrote" in files)
        read_res = q.command("cat smptest", "multicore")
        check("and gives back what was written", "multicore" in read_res)

        # The work distributed across the cores: the result is to match bit
        # for bit the one computed on a single core
        work = q.command("smp", "Cores that counted", timeout=90)
        check("the result of the divided work matches the single-core one",
              "matches" in work and "DOES NOT MATCH" not in work)

        m = re.search(r"Cores that counted: (\d+)", work)
        check("several cores counted the work at once",
              m is not None and int(m.group(1)) >= 2)

        # How many cores really took work. We do not demand that EVERY
        # one took it: an idle core stands on a hlt instruction and is
        # woken only by an interrupt, and how quickly that arrives
        # depends on the virtual machine underneath. That is a race, and
        # races are not assertions.
        shares = values(work, "Share:")
        check("more than one core took the work (%s)" % shares,
              len(shares) == 4 and sum(1 for u in shares if u) >= 2)
        check("in all exactly as many units were counted as were asked for",
              sum(shares) == 64)

        # --- Every core's own clock -------------------------------
        # A secondary core cannot use the PIT: that interrupt goes
        # through the PIC to ONE processor. What it does have is its own
        # clock in the local APIC - and only that gives it the
        # interrupts without which it could never be preempted.
        clocks = values(work, "Core clocks:")
        check("every secondary core has a clock of its own that ticks (%s)"
              % clocks,
              len(clocks) == 4 and all(z > 0 for z in clocks[1:]))
        check("the boot core runs off the PIT, not the APIC",
              len(clocks) == 4 and clocks[0] == 0)

        # A single reading proves nothing - the counters are to GROW
        time.sleep(1.5)
        again = values(q.command("smp", "Core clocks", timeout=90),
                         "Core clocks:")
        check("the secondary cores' clocks keep running (%s -> %s)"
              % (clocks, again),
              len(again) == 4 and
              all(again[i] > clocks[i] for i in range(1, 4)))

        # The threads' counters keep growing - the scheduler did not stop
        def counter():
            m = re.findall(r"thread-A: (\d+)", q.screen())
            return int(m[-1]) if m else None

        a1 = counter()
        time.sleep(1.5)
        a2 = counter()
        check("the scheduler is still running (%s -> %s)" % (a1, a2),
              a1 is not None and a2 is not None and a2 > a1)
    finally:
        q.close()
        for f in (slog, disk):
            if os.path.exists(f):
                os.unlink(f)

    # --- Long mode ---
    # A secondary core starts in real mode even when the rest of the
    # system has long been running in 64 bits: it has to walk the whole
    # road itself, sixteen bits -> thirty-two -> sixty-four. It takes the
    # page tables ready-made, the same ones the kernel uses.
    print("--- the other processors in long mode ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
    else:
        d = tempfile.mkdtemp()
        iso = tempfile.mktemp(suffix=".iso")
        slog64 = tempfile.mktemp(suffix=".log")
        try:
            check("the disc with the 64-bit kernel builds", make_iso64(d, iso))
            if os.path.exists(iso):
                q64 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog64, cpus=4,
                           qemu="qemu-system-x86_64")
                try:
                    st = q64.wait_serial("Processors:", timeout=90)
                    m = re.search(r"Processors: (\d+) \(woken (\d+)", st)
                    check("the 64-bit kernel lists every processor",
                          m is not None and int(m.group(1)) == 4)
                    check("the secondary cores reach long mode by themselves",
                          m is not None and int(m.group(2)) == 3)

                    q64.wait_serial("thread64>", timeout=40)
                    prog = q64.command("run hello64", "exit code",
                                         timeout=40)
                    check("a ring 3 program runs as before",
                          "[hello64] ending politely" in prog)

                    work = q64.command("smp", "Cores that counted",
                                          timeout=90)
                    check("the work is divided between the cores in long "
                          "mode too", "matches" in work and
                          "DOES NOT MATCH" not in work)
                    m2 = re.search(r"Cores that counted: (\d+)", work)
                    check("several cores counted the work at once (long mode)",
                          m2 is not None and int(m2.group(1)) >= 2)

                    # The core's own clock in long mode.
                    #
                    # It cost a separate bug, and one that cannot be seen
                    # from the side: a secondary core did not enable the
                    # no-execute bit in EFER for itself. The kernel maps
                    # hardware registers with that bit, and without NXE
                    # bit 63 of a page table entry is RESERVED - so the
                    # first reach for the local APIC ended in a page
                    # fault and the quiet death of the whole machine.
                    clock = values(work, "Core clocks:")
                    check("every secondary core has a clock of its own "
                          "in long mode (%s)" % clock,
                          len(clock) == 4 and all(z > 0 for z in clock[1:]))
                    time.sleep(1.5)
                    clock2 = values(q64.command("smp", "Core clocks",
                                                  timeout=90),
                                    "Core clocks:")
                    check("and they keep ticking (%s -> %s)" % (clock, clock2),
                          len(clock2) == 4 and
                          all(clock2[i] > clock[i] for i in range(1, 4)))
                finally:
                    q64.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)
            for f in (iso, slog64):
                if os.path.exists(f):
                    os.unlink(f)

    # --- One core: nothing changes ---
    print("--- a single-core machine ---")
    slog1 = tempfile.mktemp(suffix=".log")
    q1 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog1)
    try:
        start = q1.wait_for("Processors:", timeout=60)
        check("on one core the system says so outright and wakes nobody",
              "Processors: 1" in start)
        q1.wait_for("ThreadOS shell", timeout=30)
        up = q1.command("uptime", "timer ticks")
        check("and it works just the same", "timer ticks" in up)
    finally:
        q1.close()
        if os.path.exists(slog1):
            os.unlink(slog1)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
