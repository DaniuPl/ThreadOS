#!/usr/bin/env python3
"""ThreadOS smoke test: detecting and waking the remaining processors.

After power on one core works - the one the firmware chose as the
bootstrap. The rest stand and wait for a signal, and that is the
hardware's decision, not the system's: a processor woken by a SIPI
signal starts in real mode, sixteen bits wide, at the address
(vector << 12) - exactly the way a PC started forty years ago. It
starts there even when the rest of the system has long been running in
64 bits, so it has to walk the whole road itself: sixteen bits ->
thirty-two -> sixty-four. It takes the page tables ready-made, the same
ones the kernel uses.

The 'smp' command gives the woken cores countable work: a shared queue
of items, from which each takes the next number under a lock. That is
all the state the attempt shares between the cores - one number and one
lock.

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
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64


def values(text, label):
    """The numbers from ONE line of the 'smp' output.

    The command gives two lines of the same shape - the share of the
    work and the ticks of the cores' own clocks - so looking for
    'coreN=' across the whole text would glue one to the other."""
    for w in text.replace("\r", "").split("\n"):
        if w.startswith(label):
            return [int(x) for x in re.findall(r"core\d+=(\d+)", w)]
    return []


def answer(q, cmd, last, timeout=30):
    """Types a command and waits for the line starting with 'last' to
    arrive WHOLE. The port is read while the kernel is still writing,
    and half a line parses as a different answer."""
    start = len(q.serial())
    q.type_line(cmd)
    deadline = time.time() + timeout
    out = ""
    while time.time() < deadline:
        out = q.serial()[start:]
        at = out.find(last)
        if at >= 0 and "\n" in out[at:]:
            break
        time.sleep(0.3)
    return out


def smp(q):
    # 'Page faults:' is the last line the command prints
    return answer(q, "smp", "Page faults:", timeout=90)


def ticks(q):
    m = re.search(r"Uptime: \d+ s \((\d+) ticks of 10 ms\)",
                  answer(q, "uptime", "Uptime:"))
    return int(m.group(1)) if m else None


def timer_runs(q, timeout=30):
    """(first reading, the later one that is higher) - polled, because a
    clock that ticks has to be seen ticking, not merely read once."""
    first = ticks(q)
    later = None
    deadline = time.time() + timeout
    while first is not None and time.time() < deadline:
        later = ticks(q)
        if later is not None and later > first:
            break
        time.sleep(0.5)
    return first, later


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    # --- Four cores ---
    print("--- the other processors (INIT-SIPI) ---")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(DISK_SIZE)
    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, cpus=4)
    try:
        start = q.wait_serial("thread64>", timeout=90)
        m = re.search(r"Processors: (\d+) \(woken (\d+)", start)
        check("the system lists every processor from the ACPI table",
              m is not None and int(m.group(1)) == 4)
        check("the secondary cores reach long mode by themselves",
              m is not None and int(m.group(2)) == 3)

        # After waking the cores the system is to work as it did before
        t1, t2 = timer_runs(q)
        check("the timer ticks once the other cores are woken (%s -> %s)"
              % (t1, t2), t1 is not None and t2 is not None and t2 > t1)

        prog = q.command("run hello64", "exit code", timeout=40)
        check("a ring 3 program runs as before",
              "[hello64] ending politely" in prog)

        files = answer(q, "write smptest multicore", "write:")
        check("the disk works as before",
              "write: 'smptest' written (10 B)" in files)
        read_res = q.command("cat smptest", "multicore")
        check("and gives back what was written", "multicore" in read_res)

        # The work distributed across the cores: the result is to match
        # bit for bit the one computed on a single core
        work = smp(q)
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
        #
        # It cost a separate bug, and one that cannot be seen from the
        # side: a secondary core did not enable the no-execute bit in
        # EFER for itself. The kernel maps hardware registers with that
        # bit, and without NXE bit 63 of a page table entry is RESERVED
        # - so the first reach for the local APIC ended in a page fault
        # and the quiet death of the whole machine.
        clocks = values(work, "Core clocks:")
        check("every secondary core has a clock of its own that ticks (%s)"
              % clocks,
              len(clocks) == 4 and all(z > 0 for z in clocks[1:]))
        check("the boot core runs off the PIT, not the APIC",
              len(clocks) == 4 and clocks[0] == 0)

        # A single reading proves nothing - the counters are to GROW
        again = []
        deadline = time.time() + 60
        while len(clocks) == 4 and time.time() < deadline:
            again = values(smp(q), "Core clocks:")
            if len(again) == 4 and all(again[i] > clocks[i]
                                       for i in range(1, 4)):
                break
            time.sleep(0.5)
        check("the secondary cores' clocks keep running (%s -> %s)"
              % (clocks, again),
              len(again) == 4 and
              all(again[i] > clocks[i] for i in range(1, 4)))

        # The scheduler still runs after all this: a program is created,
        # run and reaped again
        prog = q.command("run hello64", "[hello64] ending", timeout=40)
        check("a ring 3 program still runs after the divided work",
              "[hello64] hello" in prog and "[hello64] ending" in prog)
    finally:
        q.close()
        if os.path.exists(disk):
            os.unlink(disk)

    # --- One core: nothing changes ---
    print("--- a single-core machine ---")
    q1 = Qemu(KERNEL, INITRD, append=TEXT_MODE)
    try:
        start = q1.wait_serial("thread64>", timeout=90)
        # The whole line: a machine that woke anybody says how many
        check("on one core the system says so outright and wakes nobody",
              re.search(r"Processors: 1\r?\n", start) is not None)
        t1, t2 = timer_runs(q1)
        check("and it works just the same (%s -> %s)" % (t1, t2),
              t1 is not None and t2 is not None and t2 > t1)
    finally:
        q1.close()

    return summary()


if __name__ == "__main__":
    sys.exit(main())
