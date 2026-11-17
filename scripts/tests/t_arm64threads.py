#!/usr/bin/env python3
"""ThreadOS smoke test: the ARM64 kernel's third stage - memory, threads
and every core.

Three machines - one core; two cores woken through PSCI's hvc; four
cores started in EL2, so PSCI's call is smc - and on each:

  the pages: as many as the RAM QEMU was given. Between the 256 MiB
  and the 1 GiB machine the free pages must differ by exactly the 768
  MiB between them, 196608 pages - a count from the wrong range, or
  pages lost to the allocator, would not
  the heap (kernel/heap.c, the x86 kernels' own): 2000 blocks of many
  sizes, each filled with its own byte and read back, none overlapping,
  and nothing left in use when all are freed
  the cores: every one the device tree lists comes up through PSCI,
  by the conduit the tree names
  the threads: four that never yield, so only the timer can take the
  core from them; each must get a real share of the time (at least a
  tenth), together they must have run on every core there is, the main
  thread's half-second sleep must end on time though nobody yields to
  it, and once joined they must give back every byte of the heap

Needs qemu-system-aarch64 and the image from `make kernelarm64`.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

MACHINES = [
    # machine, cpu, MiB, cores, PSCI conduit
    ("virt", "cortex-a72", 256, 1, "hvc"),
    ("virt", "cortex-a72", 256, 2, "hvc"),
    ("virt,virtualization=on", "cortex-a53", 1024, 4, "smc"),
]


def free_pages(said):
    m = re.search(r"^Pages: (\d+) free of (\d+), (\d+) under", said, re.M)
    return tuple(int(g) for g in m.groups()) if m else (None, None, None)


def main():
    print("--- the ARM64 kernel: memory, threads, every core ---")
    why = noarm64()
    if why:
        print("SKIPPED: " + why)
        return 0

    # Everything from the start of RAM (the device tree, the gap below
    # the kernel) to the end of the kernel's .bss must never be handed
    # out: counted here from the ELF file, not from the kernel
    end = arm64_image_end()
    under = (end - 0x40000000 + 4095) // 4096 if end else None

    pages = {}
    for machine, cpu, mib, cores, conduit in MACHINES:
        what = "%d MiB, %d core%s" % (mib, cores, "s" if cores > 1 else "")
        said = arm64_boot(machine, cpu, mib, cores)

        free, total, reserved = free_pages(said)
        pages[mib] = free
        check("[%s] the page count is the RAM's (%s pages of 4 KiB)"
              % (what, total), total == mib * 256)
        check("[%s] the pages up to the kernel's end are kept back (%s, "
              "the ELF file says %s)" % (what, reserved, under),
              reserved == under and free == total - reserved)
        check("[%s] the heap hands out 2000 blocks without overlap or loss "
              "('%s')" % (what, said_line(said, "Heap: 2000")),
              "Heap: 2000 allocations, 0 refused, 0 overlapping, 0 bytes "
              "still in use after freeing them all" in said.splitlines())
        check("[%s] and refuses sizes its arithmetic would wrap on, and "
              "more than it has ('%s')"
              % (what, said_line(said, "Heap: impossible")),
              "Heap: impossible sizes given 0 times of 3" in said.splitlines())

        want = "Cores online: %d (started through PSCI, %s)" % (cores, conduit)
        check("[%s] every core comes up ('%s')"
              % (what, said_line(said, "Cores online")),
              want in said.splitlines())

        m = re.search(r"^Threads: shares((?: \d+%)+)$", said, re.M)
        shares = [int(s) for s in m.group(1).replace("%", "").split()] \
            if m else []
        check("[%s] four threads that never yield all get a share (%s)"
              % (what, shares),
              len(shares) == 4 and min(shares) >= 10)
        want = "Threads ran on cores: " + " ".join(str(c)
                                                   for c in range(cores))
        check("[%s] and between them ran on every core ('%s')"
              % (what, said_line(said, "Threads ran on")),
              want in said.splitlines())
        m = re.search(r"woke after (\d+) ms by the counter", said)
        slept = int(m.group(1)) if m else None
        # Woken by the first tick after the deadline, then waiting at
        # most a turn behind each of the four
        check("[%s] the main thread's 500 ms sleep ends on time (%s ms)"
              % (what, slept), slept is not None and 500 <= slept <= 650)
        check("[%s] all four joined and the heap given back ('%s')"
              % (what, said_line(said, "Threads: all")),
              "Threads: all 4 joined, 0 bytes of the heap not given back"
              in said.splitlines())
        check("[%s] the kernel gets to the end" % what, ARM64_READY in said)

    diff = (pages[1024] - pages[256]) if None not in pages.values() else None
    check("the 1 GiB machine has exactly 768 MiB more free pages (%s)"
          % diff, diff == 768 * 256)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
