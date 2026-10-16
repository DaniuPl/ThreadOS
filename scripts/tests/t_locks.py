#!/usr/bin/env python3
"""ThreadOS smoke test: locks on the kernel's shared structures.

The secondary cores could so far only compute on what they were given -
all the state shared between them was one number and one lock. For them
ever to be able to run tasks, they first have to be able to CALL THE
KERNEL safely; and the kernel has to withstand it.

The 'smpmem' command takes the two structures everything else reaches
for - the physical frame map and the kernel heap - and tells every core
to allocate from them and give back three thousand times at once.

The check does not rest on whether it fell over: the answer to that is
sometimes accidental. Every allocation gets a pattern that depends on
the item's number and is read back right afterwards - a frame handed to
two cores at once has two writers and the pattern does not match. At the
end we count the frames and the heap bytes: the same before as after.

This attempt really settles something. Without a lock in the frame map
it gives dozens of pattern mismatches and dozens of frames that did not
come back; without a lock on the heap the kernel dies on a page fault,
because the free block list stops holding together.

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


def verify(out, suffix, ncores):
    check("the attempt ran to the end" + suffix, "Cores that counted:" in out)
    if "Cores that counted:" not in out:
        return

    m = re.search(r"Allocations: (\d+), failed: (\d+)", out)
    check("every allocation worked" + suffix,
          m is not None and int(m.group(1)) == 64 * 48 and
          int(m.group(2)) == 0)

    # The heart of it: a frame handed out twice has two writers
    check("every allocation went to exactly one core" + suffix,
          "Pattern: matches (errors 0)" in out)

    ram = re.search(r"Frames: (\d+) -> (\d+), heap: (\d+) -> (\d+) B", out)
    check("the frame map and the heap came back to what they were before" + suffix,
          ram is not None and ram.group(1) == ram.group(2) and
          ram.group(3) == ram.group(4))
    check("the kernel says itself that nothing leaked" + suffix,
          "Nothing leaked: yes" in out)

    check("the machine sees %d %s%s"
          % (ncores, "core" if ncores == 1 else "cores", suffix),
          re.search(r"Cores: %d \(woken %d\)" % (ncores, ncores - 1),
                    out) is not None)

    # How many cores really took work. We do not demand that EVERY one
    # took it: the bootstrap core is running the shell's thread at that
    # time and is sometimes preempted by the timer, while the secondary
    # ones spin in a tight loop and take the job apart before it can
    # reach for it. That is a race, and races are not assertions.
    share = [int(v) for v in re.findall(r"core\d+=(\d+)", out)]
    check(("the only core counted the whole set (%s)%s" if ncores == 1
           else "more than one core took the work (%s)%s")
          % (share, suffix),
          len(share) == ncores and
          (ncores == 1 or sum(1 for v in share if v) >= 2))

    # This is the right question to ask about concurrency: not "did
    # anything get computed", but was EVERY ITEM computed EXACTLY ONCE
    check("all 64 units were counted exactly once" + suffix,
          sum(share) == 64)


def main():
    print("--- the locks in the 64-bit kernel (four cores) ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
    else:
        d = tempfile.mkdtemp()
        iso = tempfile.mktemp(suffix=".iso")
        slog = tempfile.mktemp(suffix=".log")
        try:
            check("the disc with the 64-bit kernel builds", make_iso64(d, iso))
            if not os.path.exists(iso):
                return summary()

            q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, cpus=4,
                     qemu="qemu-system-x86_64")
            try:
                q.wait_serial("thread64>", timeout=90)
                out = q.command("smpmem", "Cores that counted", timeout=120)
                verify(out, "", 4)

                # The system is to live on after all this: if the locks
                # left a broken heap behind them, the next program would
                # have nothing to come into being from
                after_run = q.command("run hello64", "exit code", timeout=60)
                check("the kernel still runs after the attempt",
                      "hello, I am a" in after_run)
            finally:
                q.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)
            for f in (iso, slog):
                if os.path.exists(f):
                    os.unlink(f)

    print("--- the locks in the 32-bit kernel (four cores) ---")
    slog32 = tempfile.mktemp(suffix=".log")
    try:
        q3 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog32, cpus=4)
        try:
            q3.wait_for("ThreadOS shell", timeout=40)
            out = q3.command("smpmem", "Cores that counted", timeout=120)
            verify(out, " (32-bit)", 4)
            after_run = q3.command("run hello", "has ended", timeout=60)
            check("the kernel still runs after the attempt (32-bit)",
                  "has ended (code 0)" in after_run)
        finally:
            q3.close()
    finally:
        if os.path.exists(slog32):
            os.unlink(slog32)

    # --- A single-core machine ---
    # The same code, one core: the lock is to be harmless where there is
    # nobody to compete with
    print("--- the same attempt on one core ---")
    slog1 = tempfile.mktemp(suffix=".log")
    try:
        q1 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog1)
        try:
            q1.wait_for("ThreadOS shell", timeout=40)
            out = q1.command("smpmem", "Cores that counted", timeout=120)
            verify(out, " (one core)", 1)
        finally:
            q1.close()
    finally:
        if os.path.exists(slog1):
            os.unlink(slog1)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
