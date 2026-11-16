#!/usr/bin/env python3
"""ThreadOS smoke test: the ARM64 kernel's first stage, on QEMU's virt.

The kernel finds out what machine it is on and says so on the serial
port. That is something it can get wrong, and every later stage stands
on it. So the machine is built twice, as two different machines, and
what the kernel reports has to follow:

  the level it was entered at, and that it left EL2 for EL1 when QEMU
  started it in EL2 (virtualization=on)
  the core, read from MIDR_EL1 - a Cortex-A72 and then a Cortex-A53
  the memory and the number of cores, read from the device tree QEMU
  builds for exactly the -m and -smp it was given

A kernel that printed fixed numbers, or read the wrong cells of the
tree, fails the second machine if not the first.

Needs qemu-system-aarch64 (the qemu-system-arm package) and the image
from `make kernelarm64`, which lives next to the x86 images.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

MACHINES = [
    # machine, cpu, MiB, cores, entered at, the name the kernel must give
    ("virt", "cortex-a72", 256, 2, 1, "Cortex-A72"),
    ("virt,virtualization=on", "cortex-a53", 1024, 4, 2, "Cortex-A53"),
]


def main():
    print("--- the ARM64 kernel on QEMU's virt machine ---")
    why = noarm64()
    if why:
        print("SKIPPED: " + why)
        return 0

    for machine, cpu, mib, cores, el, name in MACHINES:
        what = "%s, %d MiB, %d cores%s" % (cpu, mib, cores,
                                           ", from EL2" if el == 2 else "")
        said = arm64_boot(machine, cpu, mib, cores)
        check("[%s] the kernel starts and says so" % what,
              "ThreadOS on ARM64" in said)
        want = "Entered at EL%d, running at EL1" % el
        check("[%s] '%s' ('%s')" % (what, want, said_line(said, "Entered")),
              want in said)
        check("[%s] the core is named from MIDR_EL1 ('%s')"
              % (what, said_line(said, "CPU:")),
              said_line(said, "CPU:").startswith("CPU: Arm " + name + " "))
        check("[%s] the device tree was found ('%s')"
              % (what, said_line(said, "Device tree:")),
              "Device tree: " in said and "found at" in said)
        want = "Memory: %d MiB at 0x40000000" % mib
        check("[%s] the memory is what QEMU was given ('%s')"
              % (what, said_line(said, "Memory:")), want in said.splitlines())
        want = "Cores: %d" % cores
        check("[%s] and so are the cores ('%s')"
              % (what, said_line(said, "Cores:")), want in said.splitlines())
        check("[%s] the kernel gets to the end" % what, ARM64_READY in said)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
