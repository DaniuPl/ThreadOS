#!/usr/bin/env python3
"""Test dymny ProtonOS: stronicowanie na zadanie.

Sterta i obszar mmap sa procesowi OBIECANE, a nie dane: prosba o 64 MiB
nie ma kosztowac ani jednej ramki. Placi sie dopiero za strony, ktorych
program naprawde dotknal - i dokladnie za tyle, ile dotknal.

Trzecia asercja pilnuje przypadku, ktory latwo przeoczyc: bufor
obiecany, ale nietkniety przez program, podany jadru w wywolaniu
systemowym. Jadro musi go najpierw urzeczywistnic, a nie uznac za zly
wskaznik.

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
from ptest import check, summary, pmm_used


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    slog = tempfile.mktemp(suffix=".log")

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
    try:
        q.wait_for("ProtonOS shell")
        q.wait_for("[siec] DHCP", timeout=20)
        time.sleep(0.5)

        q.type_line("clear")
        q.type_line("mem")
        used_before_lz = pmm_used(q.screen())
        q.type_line("run lazymem")
        screen = q.wait_for("[lazymem] dotkniete", timeout=40)
        m = re.search(r"poproszone 64 MiB, RAM urosl o (\d+) KiB", screen)
        check("prosba o 64 MiB sterty nie kosztuje ani ramki"
              + (f" (+{m.group(1)} KiB)" if m else ""),
              m is not None and int(m.group(1)) == 0)
        m = re.search(r"dotkniete 1 MiB, RAM urosl o (\d+) KiB", screen)
        check("place sie dokladnie za dotkniete strony"
              + (f" (+{m.group(1)} KiB)" if m else ""),
              m is not None and int(m.group(1)) == 1024)
        screen = q.wait_for("[lazymem] mmap tez daje strony", timeout=30)
        check("syscall urzeczywistnia nietknieta strone zamiast zglaszac blad",
              "syscall urzeczywistnil nietknieta strone" in screen)
        check("mmap tez przydziela strony dopiero przy dotknieciu",
              "[lazymem] mmap tez daje strony" in screen)
        q.wait_for("proces 'lazymem' zakonczyl sie (kod 0)", timeout=20)
        time.sleep(0.6)
        q.type_line("mem")
        used_after_lz = pmm_used(q.screen())
        check(f"brak wycieku ramek po stronicowaniu na zadanie"
              f" ({used_before_lz} -> {used_after_lz})",
              used_after_lz is not None and used_after_lz == used_before_lz)
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
