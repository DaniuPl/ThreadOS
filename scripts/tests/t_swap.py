#!/usr/bin/env python3
"""Test dymny ProtonOS: wymiana stron na dysk.

Maszyna z 16 MiB RAM-u i program, ktory bierze 18 MiB: bez wymiany nie
ma prawa sie udac. Wzorzec zalezy od numeru strony, wiec strona
wczytana z niewlasciwego miejsca od razu sie wysypie.

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
    # --- Wymiana stron na dysk -------------------------------------
    # Maszyna z 16 MiB RAM-u i program, ktory bierze 18 MiB: bez wymiany
    # nie ma prawa sie udac. Wzorzec zalezy od numeru strony, wiec strona
    # wczytana z niewlasciwego miejsca od razu sie wysypie.
    print("--- wymiana stron na dysk ---")
    swdisk = tempfile.mktemp(suffix=".img")
    with open(swdisk, "wb") as fh:
        fh.truncate(64 * 1024 * 1024)
    q16 = Qemu(KERNEL, INITRD, swdisk, append=TEXT_MODE, ram_mib=16)
    try:
        boot = q16.wait_for("Wymiana stron:", timeout=30)
        check("plik wymiany zakladany przy starcie",
              re.search(r"Wymiana stron: \d+ KiB na dysku", boot) is not None)
        q16.wait_for("ProtonOS shell", timeout=20)
        q16.type_line("clear")
        q16.type_line("run swaptest 18")
        out = q16.wait_for("[swaptest] wszystkie", timeout=120)
        check("18 MiB zmiescilo sie na maszynie z 16 MiB RAM-u",
              "stron wrocilo w calosci" in out)
        q16.type_line("clear")
        q16.type_line("mem")
        stats = q16.wait_for("wymiana:", timeout=20)
        m = re.search(r"wyrzuconych (\d+), sciagnietych (\d+)", stats)
        check("strony naprawde poszly na dysk i wrocily"
              + (f" (-{m.group(1)} +{m.group(2)})" if m else ""),
              m is not None and int(m.group(1)) > 1000
              and int(m.group(2)) > 1000)
        # Program skonczyl sie, wiec jego miejsca w pliku wymiany wrocily
        m = re.search(r"wymiana:\s+(\d+)/(\d+) stron", stats)
        check("miejsca w pliku wymiany zwolnione po smierci procesu"
              + (f" ({m.group(1)}/{m.group(2)})" if m else ""),
              m is not None and int(m.group(1)) == 0)
    finally:
        q16.close()
        if os.path.exists(swdisk):
            os.unlink(swdisk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
