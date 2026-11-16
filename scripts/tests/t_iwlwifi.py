#!/usr/bin/env python3
"""ThreadOS test: the Intel Wireless 3160 driver, against a model of the card.

No emulator has this card, so the card is modelled on the host instead
(tests/hw/iwl_model.c, tests/hw/iwl/), from the two specifications the
driver was written from - the PCIe transport and the firmware's init
sequence - and not from the driver. The model reads the real firmware
file itself to know what must be loaded where. The driver's own source,
compiled as it is with the address and undefined-behaviour sanitizers,
is run against it four times:

  ok        the whole bring-up: the card prepared, the INIT image loaded
            and calibrated, the NVM read (a known MAC, a known channel
            set), the runtime image loaded and given the PHY DB back and
            the bring-up commands; then 300 notifications through the
            256-buffer receive ring while the driver polls;
  nofw-ack  the card never signals a firmware chunk done: the driver
            gives up and says where;
  assert    the runtime firmware crashes at its first command: the driver
            reads the error log and prints the error id, its name, pc and
            blink;
  rfkill    the radio switch is off: the driver says so and stops.

Every rule of the specifications the driver breaks on the way (an access
without NIC access, a chunk at the wrong address, a command of the wrong
size, a write pointer not a multiple of 8, ...) is a failed check of its
own, named after the rule.

And one check on the build: the initrd given on the command line holds
iwl3160.ucode, byte for byte the file in third_party/iwlwifi.
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import check, summary, INITRD64   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HW = os.path.join(ROOT, "tests", "hw")
UCODE = os.path.join(ROOT, "third_party", "iwlwifi", "iwlwifi-3160-17.ucode")

# (argument, what it is, the least number of ok/FAIL lines a full run prints)
SCENARIOS = (
    ("ok", "everything right", 34),
    ("nofw-ack", "a firmware chunk never acknowledged", 10),
    ("assert", "the runtime firmware crashes", 14),
    ("rfkill", "the radio switch off", 10),
)


def build(work):
    """The driver in a tree shaped like the repository's, next to the
    model's headers: its '#include "include/..."' lines find them"""
    kdir = os.path.join(work, "kernel")
    inc = os.path.join(kdir, "include")
    os.makedirs(inc)
    os.makedirs(os.path.join(work, "kernel64"))
    for name in os.listdir(os.path.join(HW, "iwl", "include")):
        shutil.copy(os.path.join(HW, "iwl", "include", name), inc)
    shutil.copy(os.path.join(ROOT, "kernel", "include", "iwlwifi.h"), inc)
    shutil.copy(os.path.join(HW, "iwl", "paging64.h"),
                os.path.join(work, "kernel64"))
    driver = os.path.join(ROOT, "kernel", "iwlwifi.c")
    if not os.path.exists(driver):
        print("there is no kernel/iwlwifi.c")
        return None
    shutil.copy(driver, kdir)
    shutil.copy(os.path.join(HW, "iwl_model.c"), work)
    os.makedirs(os.path.join(work, "iwl"))
    for name in ("card.h", "card.c", "fw.c"):
        shutil.copy(os.path.join(HW, "iwl", name), os.path.join(work, "iwl"))
    binary = os.path.join(work, "model")
    out = subprocess.run(
        ["gcc", "-std=gnu11", "-Wall", "-Wextra", "-g", "-O1",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
         "-I", work, "-o", binary,
         os.path.join(kdir, "iwlwifi.c"), os.path.join(work, "iwl_model.c"),
         os.path.join(work, "iwl", "card.c"), os.path.join(work, "iwl", "fw.c")],
        capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr[:4000])
        return None
    if out.stderr.strip():
        # Warnings are the driver's to fix: shown, not failed here (the
        # kernel build is where they count)
        print(out.stderr[:3000])
    return binary


def initrd_files(path):
    """The TIRD format of scripts/mkinitrd.py: "TIRD", u32 count, then
    count entries {name[16], u32 offset, u32 size}, then the data"""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"TIRD":
        return None
    count = struct.unpack_from("<I", data, 4)[0]
    files = {}
    for i in range(count):
        name, off, size = struct.unpack_from("<16sII", data, 8 + 24 * i)
        files[name.rstrip(b"\0").decode(errors="replace")] = data[off:off + size]
    return files


def check_initrd():
    if not INITRD64 or not os.path.exists(INITRD64):
        print(f"SKIPPED: the initrd check - no initrd image ({INITRD64})")
        return
    files = initrd_files(INITRD64)
    check("the initrd is in the TIRD format", files is not None)
    if files is None:
        return
    with open(UCODE, "rb") as f:
        want = f.read()
    got = files.get("iwl3160.ucode")
    check("the initrd holds iwl3160.ucode", got is not None)
    check("iwl3160.ucode in the initrd is byte for byte "
          "third_party/iwlwifi/iwlwifi-3160-17.ucode "
          f"({len(want)} bytes)", got == want)


def main():
    print()
    print("--- the Intel Wireless 3160 driver against a model of the card ---")
    check_initrd()
    work = tempfile.mkdtemp()
    try:
        binary = build(work)
        check("the driver builds on the host, with the sanitizers",
              binary is not None)
        if not binary:
            return summary()
        for scenario, name, least in SCENARIOS:
            try:
                out = subprocess.run([binary, scenario, UCODE],
                                     capture_output=True, text=True,
                                     timeout=300)
            except subprocess.TimeoutExpired:
                check(f"{name}: the driver against the model finishes "
                      f"(it hung in a loop that lets no card time pass)",
                      False)
                continue
            text = out.stdout + out.stderr
            lines = [ln for ln in out.stdout.splitlines()
                     if ln.startswith(("ok: ", "FAIL: "))]
            for ln in lines:
                check(f"{name}: {ln.split(': ', 1)[1]}", ln.startswith("ok"))
            sane = "runtime error" not in text and "Sanitizer" not in text
            check(f"{name}: every check ran, and nothing the sanitizers saw "
                  f"({len(lines)} checks)",
                  len(lines) >= least and sane and out.returncode in (0, 1))
            if out.returncode != 0 or not sane:
                print(text[-6000:])
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
