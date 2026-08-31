#!/usr/bin/env python3
"""ProtonOS smoke test: paging on demand.

The heap and the mmap area are PROMISED to a process, not given: asking
for 64 MiB is not to cost a single frame. Payment is only for the pages
the program really touched - and for exactly as many as it touched.

The third assertion looks after a case that is easy to miss: a buffer
promised but untouched by the program, passed to the kernel in a system
call. The kernel has to make it real first rather than treat it as a bad
pointer.

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
