#!/usr/bin/env python3
"""Test dymny ProtonOS: dysk za kontrolerem AHCI (SATA).

Ten sam system plikow i ta sama wymiana stron, tyle ze dane przenosi
kontroler przez DMA zamiast procesora slowo po slowie.

Uruchamiany osobno albo przez scripts/run_tests.py, ktory puszcza
wszystkie grupy rownolegle.
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
    # --- Dysk za kontrolerem AHCI ----------------------------------
    # Ten sam system plikow i ta sama wymiana stron, tyle ze dane
    # przenosi kontroler przez DMA zamiast procesora slowo po slowie.
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

        # Wymiana stron przez AHCI: te same 18 MiB na maszynie z 16 MiB
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
