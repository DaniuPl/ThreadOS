#!/usr/bin/env python3
"""ThreadOS smoke test: the ARM64 kernel's second stage - exceptions,
the MMU, the interrupt controller and the timer.

Each of them is asked to do the one thing that proves it is there:

  the vector table: a breakpoint (selftest=brk) is caught, named with
  its number, stepped over, and the kernel carries on with a value it
  left in x9 intact - which only the vector's saving and restoring of
  every register can do
  the MMU: address 0x10 is the flash on virt, readable with the MMU
  off; with it on and the first 2 MiB left unmapped, reading it
  (selftest=abort) must end in a data abort naming that address and a
  translation fault at level 2, and nothing after it may run; and no
  RAM but the kernel's own may be run (UXN everywhere, PXN outside the
  kernel's gigabyte): with 2 GiB, a 'ret' written into the last page
  and called (selftest=xn) ends in an instruction abort, a permission
  fault, there
  the GIC and the timer: the controller is found where QEMU's virt puts
  it (distributor 0x08000000, CPU interface 0x08010000), and a hundred
  timer interrupts take a second by the core's own counter - both from
  EL1 and from EL2. (From EL2 this proves the timer works after the way
  down, not that boot.S clears the counter offset: QEMU resets it to 0
  anyway, so leaving that out passes here. A real board may differ.)
  a GICv3 machine is recognised from the device tree and declined in
  words, not driven through a GICv2's registers

Needs qemu-system-aarch64 and the image from `make kernelarm64`.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary


def timer_ms(said):
    m = re.search(r"Timer: 100 ticks took (\d+) ms by the counter", said)
    return int(m.group(1)) if m else None


def main():
    print("--- the ARM64 kernel: exceptions, MMU, GIC, timer ---")
    why = noarm64()
    if why:
        print("SKIPPED: " + why)
        return 0

    for machine, el in (("virt", 1), ("virt,virtualization=on", 2)):
        said = arm64_boot(machine)
        what = "from EL%d" % el
        check("[%s] the MMU is on, with the caches ('%s')"
              % (what, said_line(said, "MMU on")),
              "MMU on: " in said and "(SCTLR M1 C1 I1)" in said)
        check("[%s] the GIC is found where virt has it ('%s')"
              % (what, said_line(said, "Interrupt controller")),
              "Interrupt controller: GICv2, distributor 0x8000000, "
              "CPU interface 0x8010000" in said)
        ms = timer_ms(said)
        # Never less than the hundred intervals; more only by how late
        # the last interrupt was answered
        check("[%s] a hundred timer interrupts take a second by the "
              "counter (%s ms)" % (what, ms),
              ms is not None and 1000 <= ms <= 1200)
        check("[%s] and the kernel goes on to the end" % what,
              ARM64_READY in said)

    said = arm64_boot(append="selftest=brk")
    check("a breakpoint is caught and named ('%s')"
          % said_line(said, "Breakpoint"),
          re.search(r"^Breakpoint \(BRK #0x7a\) at 0x[0-9a-f]+ - stepping "
                    r"over it$", said, re.M) is not None)
    check("the way back restores the registers ('%s')"
          % said_line(said, "Back from"),
          "Back from the breakpoint, x9 kept" in said.splitlines())
    check("and the kernel carries on to the end after it",
          ARM64_READY in said and timer_ms(said) is not None)

    said = arm64_boot(append="selftest=abort")
    check("reading an unmapped address is a data abort, named ('%s')"
          % said_line(said, "Data abort"),
          re.search(r"^Data abort: reading 0x10, translation fault at "
                    r"level 2, at 0x[0-9a-f]+ - stopping\.$", said, re.M)
          is not None)
    check("and nothing after it runs",
          "Read address 0x10 without a fault" not in said and
          ARM64_READY not in said)

    # Two gigabytes, so there is RAM outside the kernel's own to try
    said = arm64_boot(mib=2048, append="selftest=xn")
    m = re.search(r"^Running a data page at (0x[0-9a-f]+)$", said, re.M)
    check("a 'ret' written into RAM outside the kernel's gigabyte is "
          "called ('%s')" % said_line(said, "Running a data page"),
          m is not None)
    check("and ends in an instruction abort there, a permission fault - "
          "nothing in RAM but the kernel runs ('%s')"
          % said_line(said, "Instruction abort"),
          m is not None and re.search(
              r"^Instruction abort: running %s, permission fault" % m.group(1),
              said, re.M) is not None and
          "nothing stopped it" not in said)

    said = arm64_boot("virt,gic-version=3")
    check("a GICv3 is recognised and declined in words ('%s')"
          % said_line(said, "Interrupt controller"),
          "Interrupt controller: GICv3 at 0x8000000 - not supported yet, "
          "so no timer." in said)
    check("and the kernel still gets to the end", ARM64_READY in said)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
