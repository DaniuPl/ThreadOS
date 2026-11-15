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


def wait_shell(q, timeout=90):
    """The first console's prompt. The other three consoles print theirs
    earlier, so the first 'thread64>' on the port does not mean the
    shell the keyboard talks to is there."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        out = q.serial()
        at = out.find("Commands:")
        if at >= 0 and "thread64>" in out[at:]:
            return out
        time.sleep(0.3)
    raise RuntimeError("timeout: the first console's shell never came up")


def main():
    print("--- the other processors (INIT-SIPI) ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    slog1 = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(DISK_SIZE)
    try:
        check("the disc with the 64-bit kernel builds", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        # --- Four cores ---
        # A secondary core starts in real mode even when the rest of
        # the system has long been running in 64 bits: it has to walk
        # the whole road itself, sixteen bits -> thirty-two -> sixty-
        # four. It takes the page tables ready-made, the same ones the
        # kernel uses.
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, cpus=4, qemu="qemu-system-x86_64")
        try:
            st = q.wait_serial("Processors:", timeout=90)
            m = re.search(r"Processors: (\d+) \(woken (\d+)", st)
            check("the system lists every processor from the ACPI table",
                  m is not None and int(m.group(1)) == 4)
            check("the other three cores reach long mode by themselves",
                  m is not None and int(m.group(2)) == 3)

            wait_shell(q)

            # After waking the cores the system is to work as it did
            # before
            up = q.command("uptime", "Uptime:")
            m = re.search(r"Uptime: \d+ s \((\d+) ticks", up)
            check("the timer ticks once the other cores are woken",
                  m is not None and int(m.group(1)) > 0)

            prog = q.command("run hello64", "exit code", timeout=40)
            check("a ring 3 program runs as before",
                  "[hello64] ending politely" in prog)

            files = q.command("write smptest multicore", "write:")
            check("the disk works as before",
                  "write: 'smptest' written" in files)
            read_res = q.command("cat smptest", "multicore")
            check("and gives back what was written", "multicore" in read_res)

            # The work distributed across the cores: the result is to
            # match bit for bit the one computed on a single core. The
            # page fault counters are the last line of the report.
            work = q.command("smp", "Page faults:", timeout=90)
            check("the result of the divided work matches the single-core "
                  "one", "matches" in work and "DOES NOT MATCH" not in work)
            m = re.search(r"Cores that counted: (\d+)", work)
            check("several cores counted the work at once",
                  m is not None and int(m.group(1)) >= 2)

            # How many cores really took work. We do not demand that
            # EVERY one took it: an idle core stands on a hlt
            # instruction and is woken only by an interrupt, and how
            # quickly that arrives depends on the virtual machine
            # underneath. That is a race, and races are not assertions.
            shares = values(work, "Share:")
            check("more than one core took the work (%s)" % shares,
                  len(shares) == 4 and sum(1 for u in shares if u) >= 2)
            check("in all exactly as many units were counted as were "
                  "asked for", sum(shares) == 64)

            # --- Every core's own clock -------------------------------
            # A secondary core cannot use the PIT: that interrupt goes
            # through the PIC to ONE processor. What it does have is its
            # own clock in the local APIC - and only that gives it the
            # interrupts without which it could never be preempted.
            #
            # It cost a separate bug, and one that cannot be seen from
            # the side: a secondary core did not enable the no-execute
            # bit in EFER for itself. The kernel maps hardware registers
            # with that bit, and without NXE bit 63 of a page table
            # entry is RESERVED - so the first reach for the local APIC
            # ended in a page fault and the quiet death of the whole
            # machine.
            clocks = values(work, "Core clocks:")
            check("every secondary core has a clock of its own that ticks "
                  "(%s)" % clocks,
                  len(clocks) == 4 and all(z > 0 for z in clocks[1:]))
            check("the boot core runs off the PIT, not the APIC",
                  len(clocks) == 4 and clocks[0] == 0)

            # A single reading proves nothing - the counters are to GROW
            time.sleep(1.5)
            again = values(q.command("smp", "Page faults:", timeout=90),
                           "Core clocks:")
            check("the secondary cores' clocks keep running (%s -> %s)"
                  % (clocks, again),
                  len(again) == 4 and
                  all(again[i] > clocks[i] for i in range(1, 4)))

            # And the scheduler still hands the cores out: a program
            # started after all this runs to its own end
            prog = q.command("run hello64", "exit code", timeout=40)
            check("the scheduler still runs programs after the work",
                  "[hello64] done, exit code 7" in prog)
        finally:
            q.close()

        # --- One core: nothing changes ---
        print("--- a single-core machine ---")
        q1 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog1,
                  qemu="qemu-system-x86_64")
        try:
            start = q1.wait_serial("Processors:", timeout=90)
            check("on one core the system says so outright and wakes "
                  "nobody", re.search(r"Processors: 1\b", start)
                  is not None and "woken" not in start)
            wait_shell(q1)
            up = q1.command("uptime", "Uptime:")
            check("and it works just the same", "Uptime:" in up)
        finally:
            q1.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, slog1, disk):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
