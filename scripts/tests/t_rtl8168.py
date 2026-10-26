#!/usr/bin/env python3
"""ThreadOS test: the RTL8111/8168 driver, against a model of the chip.

No emulator has this card - QEMU has the older RTL8139 only - so the
card is modelled on the host instead (tests/hw/rtl8168_model.c), from
Realtek's description of it rather than from the driver, and the
driver's own source, compiled as it is with the address and
undefined-behaviour sanitizers, is run against it.

Twice: as an 8168B, the PHY up and behind PHYAR, and as an 8168GU -
the generation of 2015 laptops - its PHY powered down and behind OCP,
and its receiving gated until the driver opens it. Each time: the
card started, its address read, the PHY powered up only where it was
down; a hundred frames sent through a ring of 32 reach the wire whole
and in order, a short one padded; a hundred received through a ring of
64 reach the network code whole and in order, those for other
addresses filtered out by the card as the driver set it; a frame with
a bad CRC dropped; a burst larger than the ring losing only what did
not fit.

The kernel images given on the command line are not needed; the test
runs on the host.
"""

import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import check, summary   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HW = os.path.join(ROOT, "tests", "hw")


def build(work):
    """The driver next to stub headers: its '#include "include/io.h"'
    finds them, and every port it touches is the model's"""
    inc = os.path.join(work, "include")
    os.makedirs(inc)
    for name in os.listdir(os.path.join(HW, "include")):
        shutil.copy(os.path.join(HW, "include", name), inc)
    shutil.copy(os.path.join(ROOT, "kernel", "include", "rtl8168.h"), inc)
    shutil.copy(os.path.join(ROOT, "kernel", "rtl8168.c"), work)
    shutil.copy(os.path.join(HW, "rtl8168_model.c"), work)
    binary = os.path.join(work, "model")
    out = subprocess.run(
        ["gcc", "-std=gnu11", "-Wall", "-Wextra", "-g", "-O1",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
         "-o", binary, os.path.join(work, "rtl8168.c"),
         os.path.join(work, "rtl8168_model.c")],
        capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr[:3000])
        return None
    return binary


def main():
    print()
    print("--- the RTL8168 driver against a model of the chip ---")
    work = tempfile.mkdtemp()
    try:
        binary = build(work)
        check("the driver builds on the host, with the sanitizers",
              binary is not None)
        if not binary:
            return summary()
        for chip, bmcr, name in (("380", "1140", "8168B, its PHY up"),
                                 ("509", "1940", "8168GU, its PHY down")):
            try:
                out = subprocess.run([binary, chip, bmcr],
                                     capture_output=True, text=True,
                                     timeout=30)
            except subprocess.TimeoutExpired:
                check(f"{name}: the driver against the model finishes "
                      f"(it hung: a ring it never gets out of?)", False)
                continue
            text = out.stdout + out.stderr
            lines = [ln for ln in out.stdout.splitlines()
                     if ln.startswith(("ok: ", "FAIL: "))]
            for ln in lines:
                check(f"{name}: {ln.split(': ', 1)[1]}", ln.startswith("ok"))
            check(f"{name}: every check ran, and nothing the sanitizers "
                  f"saw ({len(lines)} checks)",
                  out.returncode == 0 and len(lines) >= 9 and
                  "runtime error" not in text and "Sanitizer" not in text)
            if out.returncode != 0:
                print(text[-3000:])
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
