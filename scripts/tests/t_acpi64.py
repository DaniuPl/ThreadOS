#!/usr/bin/env python3
"""Test dymny ProtonOS: ACPI w jadrze 64-bitowym.

Wylaczenie zasilania to jedyna rzecz, ktorej nie da sie sprawdzic
napisem na ekranie: albo maszyna gasnie, albo nie. QEMU konczy sie
wtedy sam i to jest cala asercja - reszta testu tylko doprowadza
system do momentu, w ktorym mozna o to poprosic.

Droga do tego wiedzie przez tablice firmware'u: RSDP, spis tablic,
FADT i obiekt _S5_ w bajtkodzie AML. Ten sam plik acpi.c obsluguje
oba jadra - w trybie dlugim rozni sie tylko tym, ze tablice doklejane
sa do mapowania innym kodem.
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

        # --- Tablice i wylaczenie zasilania ---
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

        # --- Restart ---
        # QEMU dostaje -no-reboot, wiec reset konczy maszyne zamiast ja
        # podnosic: to, ze proces znika, jest dowodem na sam reset.
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
