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
    print("--- kontroler AHCI (SATA) ---")
    sdisk = tempfile.mktemp(suffix=".img")
    with open(sdisk, "wb") as fh:
        fh.truncate(64 * 1024 * 1024)
    q18 = Qemu(KERNEL, INITRD, sdisk, append=TEXT_MODE, ram_mib=16, sata=True)
    try:
        boot = q18.wait_for("+ PFS gotowy", timeout=30)
        check("kontroler AHCI wykryty po PCI i wybrany zamiast ATA",
              "Dysk AHCI (SATA, DMA) + PFS gotowy" in boot)
        check("PFS sformatowal dysk przez AHCI",
              "sformatowany" in boot)
        check("plik wymiany zalozony na dysku SATA",
              re.search(r"Wymiana stron: \d+ KiB",
                        q18.wait_for("Wymiana stron:", timeout=20)) is not None)

        q18.wait_for("ProtonOS shell", timeout=20)
        q18.type_line("clear")
        q18.type_line("write ahci proton przez dma")
        q18.wait_for("zapisano", timeout=20)
        q18.type_line("cat ahci")
        back = q18.wait_for("proton przez dma", timeout=20)
        check("zapis i odczyt pliku przez DMA oddaja te same dane",
              "proton przez dma" in back)

        # Page swapping through AHCI: the same 18 MiB on a machine with 16 MiB
        q18.type_line("clear")
        q18.type_line("run swaptest 18")
        out = q18.wait_for("[swaptest] wszystkie", timeout=120)
        check("wymiana stron przez AHCI oddaje strony bez zmian",
              "stron wrocilo w calosci" in out)
    finally:
        q18.close()
        if os.path.exists(sdisk):
            os.unlink(sdisk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
