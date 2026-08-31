#!/usr/bin/env python3
"""ProtonOS smoke test: ACPI in the 64-bit kernel.

Powering off is the one thing that cannot be checked with text on the
screen: either the machine goes dark or it does not. QEMU then ends by
itself and that is the whole assertion - the rest of the test only
brings the system to the point where it can be asked for it.

The road to it leads through the firmware's tables: RSDP, the table
list, the FADT and the _S5_ object in AML bytecode. The same acpi.c file
serves both kernels - in long mode the only difference is that the
tables are added to the mapping by different code.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def main():
    print("--- ACPI: wylaczanie i restart (jadro 64-bitowe) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    try:
        check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        # --- The tables and powering off ---
        slog = tempfile.mktemp(suffix=".log")
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            start = q.wait_serial("ACPI:", timeout=60)
            check("tablice ACPI znalezione przy starcie",
                  re.search(r"ACPI: tablice od '\S", start) is not None)
            q.wait_serial("proton64>", timeout=30)
            q.type_line("help")
            check("'poweroff' jest w spisie polecen",
                  "poweroff" in q.wait_serial("Polecenia:", timeout=20))
            q.type_line("poweroff")
            check("'poweroff' faktycznie gasi maszyne",
                  q.wait_exit(timeout=25))
        finally:
            q.close()
            if os.path.exists(slog):
                os.unlink(slog)

        # --- Reset ---
        # QEMU is given -no-reboot, so a reset ends the machine instead of
        # bringing it up: the process disappearing is the proof of the
        # reset itself.
        slog2 = tempfile.mktemp(suffix=".log")
        q2 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog2,
                  qemu="qemu-system-x86_64")
        try:
            q2.wait_serial("proton64>", timeout=60)
            q2.type_line("reboot")
            check("'reboot' resetuje maszyne", q2.wait_exit(timeout=25))
            check("jadro zdazylo powiedziec, ze restartuje",
                  "Restart..." in q2.serial())
        finally:
            q2.close()
            if os.path.exists(slog2):
                os.unlink(slog2)
    finally:
        shutil.rmtree(d, ignore_errors=True)
        if os.path.exists(iso):
            os.unlink(iso)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
