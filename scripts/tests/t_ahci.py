#!/usr/bin/env python3
"""ProtonOS smoke test: a disk behind an AHCI controller (SATA).

The same file system and the same page swapping, only the data are
carried by the controller through DMA instead of by the processor word
by word.

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
    # --- A disk behind an AHCI controller --------------------------
    # The same file system and the same page swapping, only the data are
    # carried by the controller through DMA instead of by the processor
    # word by word.
    print("--- the AHCI controller (SATA) ---")
    sdisk = tempfile.mktemp(suffix=".img")
    with open(sdisk, "wb") as fh:
        fh.truncate(64 * 1024 * 1024)
    q18 = Qemu(KERNEL, INITRD, sdisk, append=TEXT_MODE, ram_mib=16, sata=True)
    try:
        boot = q18.wait_for("+ PFS ready", timeout=30)
        check("the AHCI controller was found over PCI and picked instead of ATA",
              "Disk AHCI (SATA, DMA) + PFS ready" in boot)
        check("PFS formatted the disk through AHCI",
              "formatted" in boot)
        check("the swap file was created on the SATA disk",
              re.search(r"Page swapping: \d+ KiB",
                        q18.wait_for("Page swapping:", timeout=20)) is not None)

        q18.wait_for("ProtonOS shell", timeout=20)
        q18.type_line("clear")
        q18.type_line("write ahci proton through dma")
        q18.wait_for("wrote", timeout=20)
        q18.type_line("cat ahci")
        back = q18.wait_for("proton through dma", timeout=20)
        check("writing and reading a file through DMA give back the same data",
              "proton through dma" in back)

        # Page swapping through AHCI: the same 18 MiB on a machine with 16 MiB
        q18.type_line("clear")
        q18.type_line("run swaptest 18")
        out = q18.wait_for("[swaptest] all", timeout=120)
        check("swapping through AHCI gives the pages back unchanged",
              "pages came back whole" in out)
    finally:
        q18.close()
        if os.path.exists(sdisk):
            os.unlink(sdisk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
