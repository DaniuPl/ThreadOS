#!/usr/bin/env python3
"""ThreadOS smoke test: random numbers on the ARM64 kernel.

HTTPS in Weft is only as secret as its keys, and the ARM64 kernel had
nothing fit to make them from: SYS64_GETRANDOM answered -1. It now has
the PC's own pool (kernel/random.c, built for both kernels: BLAKE2s
and ChaCha20), fed on ARM64 by the generic timer's counter at every
interrupt, the processor's RNDR where there is one (QEMU's "max"), and
a virtio entropy device where there is one (kernelarm64/vrng.c).

Two boots, randtest on each (libc/tests/randtest.c, the PC's, with
"-" for its files: 64 KiB of each sample on the console), judged here
as t_random judges the PC's:

  a Cortex-A72 with a virtio entropy device: the kernel takes 128 bytes
  from the device and names interrupt timing as its other source

  QEMU's "max" processor without the device: the kernel names RNDR

  and on both: getrandom in pieces of every size and /dev/urandom have
  as many ones as zeros, every byte value and pair of values about as
  often as chance allows, no 16-byte block twice, and zlib cannot
  shrink them; GRND_NONBLOCK answers once the pool is seeded,
  getentropy works, a forked child and its parent and four threads
  drawing at once never get the same bytes; and the two boots share
  nothing
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, noarm64, arm64_boot, ARM64_INITRD
from t_random import judge

SHOWN = 64 * 1024


def one_boot(tag, cpu, extra):
    said = arm64_boot(cpu=cpu, cores=2, mib=512, initrd=ARM64_INITRD,
                      timeout=300, append="run=randtest,-,-", extra=extra)
    banner = next((l for l in said.splitlines()
                   if "Random numbers:" in l), "no line")
    start = re.search(r"^\[proc\] randtest started, pid \d+$", said, re.M)
    end = re.search(r"^\[proc\] randtest \(pid \d+\) ended with code "
                    r"(-?\d+)$", said, re.M)
    body = said[start.end():end.start()] if start and end else ""
    lines = [l for l in body.splitlines() if l]
    data = {"rand": b"", "dev": b""}
    for l in lines:
        m = re.match(r"^=(rand|dev) ([0-9a-f]+)$", l)
        if m:
            data[m.group(1)] += bytes.fromhex(m.group(2))
    plain = [l for l in lines if not l.startswith("=")]
    return said, banner, plain, int(end.group(1)) if end else None, data


def main():
    print("--- random numbers on the ARM64 kernel ---")
    why = noarm64()
    if why:
        print("SKIPPED: " + why)
        return summary()
    if not os.path.exists(ARM64_INITRD):
        print("SKIPPED: no ARM64 initrd (make kernelarm64)")
        return summary()

    results = {}
    for tag, cpu, extra, sources in (
            ("Cortex-A72 + virtio-rng", "cortex-a72",
             ["-device", "virtio-rng-device", "-global",
              "virtio-mmio.force-legacy=false"], "interrupt timing"),
            ("max", "max", [], "RNDR, interrupt timing")):
        said, banner, plain, code, data = one_boot(tag, cpu, extra)
        check(f"{tag}: the kernel names its sources as '{sources}' "
              f"({banner})", banner.endswith("sources: " + sources))
        if extra:
            check(f"{tag}: and takes the entropy device's bytes",
                  "Random: virtio entropy device, 128 bytes into the pool"
                  in said)
        for want in ("wrote 1048576 bytes to -",
                     "/dev/urandom: a character device, 262144 bytes "
                     "read into -",
                     "GRND_NONBLOCK once seeded: 16 bytes",
                     "getentropy of 256 bytes: ok",
                     "after fork: the parent's and the child's bytes differ",
                     "4 threads, 8000 draws of 16 bytes: all answered, "
                     "0 the same"):
            check(f"{tag}: randtest says '{want}'", want in plain)
        check(f"{tag}: randtest ends with code 0 (got {code})", code == 0)
        for what, key in (("getrandom", "rand"), ("/dev/urandom", "dev")):
            judge(f"{tag}, {what}", data[key], SHOWN,
                  where="its sample is on the console")
        sample = next((l.split()[1] for l in plain
                       if l.startswith("~sample ")), None)
        results[tag] = (sample, data["rand"])
    (s1, d1), (s2, d2) = results.values()
    shared = ({d1[i:i + 16] for i in range(0, len(d1), 16)} &
              {d2[i:i + 16] for i in range(0, len(d2), 16)})
    check("two boots share no 16-byte block, and their samples differ",
          bool(s1 and s2 and s1 != s2 and d1 and d2 and not shared))
    return summary()


if __name__ == "__main__":
    sys.exit(main())
