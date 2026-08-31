#!/usr/bin/env python3
"""ProtonOS smoke test: the line between the kernel and a program.

A program in ring 3 will not reach for kernel memory itself - paging
sees to that. It can, however, do it by somebody else's hands: pass a
pointer to kernel memory in a system call and count on the kernel
writing something there on its behalf, with full rights.

That is why we check both roads. 'evil' reaches directly and is to die,
and the kernel is to show the address it reached for. 'evil2' passes
such pointers in syscalls - each is to come back with an error, but the
process itself is to SURVIVE it and exit with code zero. A refusal that
kills the process would pass here too, and that is not the point.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary


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

        # 'run evil': an attempt to read kernel memory directly
        q.type_line("clear")
        q.type_line("run evil")
        screen = q.wait_for("proces 'evil' ubity")
        check("evil czytal pamiec jadra i zostal ubity",
              "proces 'evil' ubity" in screen)
        check("CR2 pokazuje atakowany adres",
              "siegnal po adres 100000" in screen)
        check("kod po ataku nie wykonal sie",
              "TEGO NIE POWINNO BYC WIDAC" not in screen)

        # 'run evil2': the same addresses, but passed to the kernel in
        # syscalls
        q.type_line("run evil2")
        ev2 = q.wait_for("granica trzyma", timeout=20)
        check("syscalle odrzucaja wskazniki na pamiec jadra",
              "jadro odrzucilo wszystkie proby" in ev2)
        check("evil2 nie wypisal pamieci jadra na konsole",
              "PRZESZLO" not in ev2 and "DZIURA" not in ev2)
        check("proces przezyl odrzucone syscalle (kod 0)",
              "kod 0" in ev2)
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
