#!/usr/bin/env python3
"""ThreadOS smoke test: programs on the ARM64 kernel, at EL0.

The ARM64 kernel's fifth stage: an AArch64 ELF file from the initrd,
loaded into an address space of its own and run at EL0, coming into the
kernel only by a system call, an interrupt or a fault
(kernelarm64/proc.c). The programs are userarm64/; the kernel starts
those its command line names (run=NAME,ARG,...) all at once and says
how each ended. What is checked, from the serial port:

  hello gets its arguments, and what it computes in its own memory -
  a megabyte of .bss and four of heap from SYS64_SBRK, each filled and
  hashed (FNV-1a), and a sum in floating point - is what Python
  computes from the same recipe; it ends with the code it returned

  two fpsums on one core, whose running times overlap - so the timer
  switched between them - each come out bit for bit as Python's sum:
  every switch carried their FP/SIMD registers along

  spin never gives the core up, yet hello, started after it on the one
  core there is, ends first: the timer takes the core from a program

  a program that reads the kernel's memory, reads address 0 or writes
  its own code is ended, with the kernel saying why (permission and
  translation faults) and code 139 - and the kernel goes on to the
  next program; a system call given the kernel's memory, memory that
  is not mapped, a file it does not have or a number that is not a
  call answers -1

  the same on four cores, four hellos and two fpsums at once, each in
  its own table on whichever core: the same answers

  and when they have all ended every page they had is free again

Needs qemu-system-aarch64 and `make kernelarm64`.
"""

import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, ARM64_INITRD


def fnv(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def bits(d):
    return "%016x" % struct.unpack("<Q", struct.pack("<d", d))[0]


def hello_answers():
    big = bytes((i * 7 + (i >> 9)) & 255 for i in range(1 << 20))
    heap = bytes((i ^ (i >> 11)) & 255 for i in range(4 << 20))
    s = 0.0
    for k in range(1, 100001):
        s += 1.0 / (float(k) * float(k))
    return "%016x" % fnv(big), "%016x" % fnv(heap), bits(s)


def fpsum(seed):
    x, acc = float(seed), 0.0
    for _ in range(3000000):
        x = x * 1.0000001 + 0.5
        if x > 1e6:
            x = x / 3.0
        acc = acc + x
    return bits(acc)


def ended(said, name, pid=None):
    """The codes the kernel said the program (by name) ended with"""
    return [int(c) for p, c in re.findall(
        r"^\[proc\] %s \(pid (\d+)\) ended with code (-?\d+)$"
        % re.escape(name), said, re.M) if pid is None or int(p) == pid]


def pages_back(said):
    m = re.search(r"pages free: (\d+) before the programs, (\d+) after",
                  said)
    return m is not None and m.group(1) == m.group(2), \
        m.group(0) if m else "nothing said"


def main():
    print("--- programs on the ARM64 kernel, at EL0 ---")
    why = noarm64()
    if why:
        print("SKIPPED: " + why)
        return summary()
    if not os.path.exists(ARM64_INITRD):
        print("SKIPPED: no ARM64 initrd (make kernelarm64)")
        return summary()
    bss, heap, fp = hello_answers()
    sums = {3: fpsum(3), 11: fpsum(11)}

    # --- one core
    said = arm64_boot(cores=1, initrd=ARM64_INITRD, timeout=120,
                      append="run=spin run=hello,one,two run=fpsum,3 "
                             "run=fpsum,11 run=fault,kernel run=fault,null "
                             "run=fault,text run=badcall")
    check("the kernel starts the eight programs and is ready after them",
          said.count("[proc] ") >= 16 and ARM64_READY in said)
    check("hello gets its arguments",
          "hello from EL0, pid 2, args hello one two" in said)
    check(f"what it computes in its .bss is Python's ({bss})",
          "bss " + bss in said)
    check(f"in its heap from SYS64_SBRK ({heap})", "heap " + heap in said)
    check(f"and in floating point ({fp})", "fp " + fp in said)
    check("and it ends with the code it returned (7)",
          ended(said, "hello") == [7])

    runs = {}
    for seed, got, t0, t1 in re.findall(
            r"^fpsum (\d+) ([0-9a-f]{16}) ticks (\d+) (\d+)$", said, re.M):
        runs[int(seed)] = (got, int(t0), int(t1))
    both = len(runs) == 2
    overlap = both and runs[3][1] < runs[11][2] and runs[11][1] < runs[3][2]
    check(f"two fpsums on one core ran at the same time ({runs})",
          overlap)
    check(f"and each sum is Python's, bit for bit ({sums})",
          both and runs[3][0] == sums[3] and runs[11][0] == sums[11])

    check("spin never gives the core up, yet hello, started after it on "
          "the one core, finishes first",
          "spin: done" in said and "fp " + fp in said and
          said.find("fp " + fp) < said.find("spin: done"))

    check("reading the kernel's memory ends the program: a permission "
          "fault",
          re.search(r"^\[proc\] fault \(pid \d+\): reading 0x40200000 "
                    r"\(permission fault\), at 0x40000\w+ - ended$",
                    said, re.M) is not None)
    check("reading address 0: a translation fault",
          re.search(r"^\[proc\] fault \(pid \d+\): reading 0x0 "
                    r"\(translation fault\)", said, re.M) is not None)
    check("writing its own code: a permission fault",
          re.search(r"^\[proc\] fault \(pid \d+\): writing 0x40000\w+ "
                    r"\(permission fault\)", said, re.M) is not None)
    check("each with code 139, and nothing stopped any of them otherwise",
          ended(said, "fault") == [139, 139, 139] and
          "nothing stopped it" not in said)
    check("system calls given what is not the program's answer -1",
          "write of the kernel's memory: -1" in said and
          "write of unmapped memory: -1" in said and
          "write to a file it does not have: -1" in said and
          "an unknown system call: -1" in said and
          ended(said, "badcall") == [0])
    ok, line = pages_back(said)
    check(f"every page the programs had is free again ({line})", ok)

    # --- four cores
    said = arm64_boot(cores=4, mib=512, initrd=ARM64_INITRD, timeout=120,
                      append="run=hello,a run=hello,b run=hello,c "
                             "run=hello,d run=fpsum,3 run=fpsum,11")
    hellos = re.findall(r"^hello from EL0, pid \d+, args hello (\w)$", said,
                        re.M)
    check(f"four cores: four hellos at once ({sorted(hellos)})",
          sorted(hellos) == ["a", "b", "c", "d"])
    check("each with Python's answers",
          said.count("bss " + bss) == 4 and said.count("heap " + heap) == 4
          and said.count("fp " + fp) == 4 and ended(said, "hello") == [7] * 4)
    got = dict((int(s), g) for s, g in
               re.findall(r"^fpsum (\d+) ([0-9a-f]{16}) ", said, re.M))
    check(f"and both fpsums right ({got})", got == sums)
    ok, line = pages_back(said)
    check(f"and every page back ({line})", ok)
    return summary()


sys.exit(main())
