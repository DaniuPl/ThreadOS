#!/usr/bin/env python3
"""ThreadOS smoke test: swapping pages out to disk.

A machine with 16 MiB of RAM and a program that takes 18 MiB: without
swapping it has no right to succeed. The pattern depends on the page
number, so a page read back from the wrong place falls apart at once.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary


def main():
    # --- Swapping pages out to disk --------------------------------
    # A machine with 16 MiB of RAM and a program that takes 18 MiB:
    # without swapping it has no right to succeed. The pattern depends on
    # the page number, so a page read back from the wrong place falls
    # apart at once.
    print("--- swapping pages to the disk ---")
    swdisk = tempfile.mktemp(suffix=".img")
    with open(swdisk, "wb") as fh:
        fh.truncate(64 * 1024 * 1024)
    q16 = Qemu(KERNEL, INITRD, swdisk, append=TEXT_MODE, ram_mib=16)
    try:
        boot = q16.wait_for("Page swapping:", timeout=30)
        check("the swap file is created at boot",
              re.search(r"Page swapping: \d+ KiB on the disk", boot) is not None)
        q16.wait_for("ThreadOS shell", timeout=20)
        q16.type_line("clear")
        q16.type_line("run swaptest 18")
        out = q16.wait_for("[swaptest] all", timeout=120)
        check("18 MiB fitted on a machine with 16 MiB of RAM",
              "pages came back whole" in out)
        q16.type_line("clear")
        q16.type_line("mem")
        stats = q16.wait_for("swap:", timeout=20)
        m = re.search(r"evicted (\d+), fetched (\d+)", stats)
        check("the pages really went to the disk and came back"
              + (f" (-{m.group(1)} +{m.group(2)})" if m else ""),
              m is not None and int(m.group(1)) > 1000
              and int(m.group(2)) > 1000)
        # The program has finished, so its slots in the swap file came back
        m = re.search(r"swap:\s+(\d+)/(\d+) pages", stats)
        check("the room in the swap file was freed once the process died"
              + (f" ({m.group(1)}/{m.group(2)})" if m else ""),
              m is not None and int(m.group(1)) == 0)
    finally:
        q16.close()
        if os.path.exists(swdisk):
            os.unlink(swdisk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
