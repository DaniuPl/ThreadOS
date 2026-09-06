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
        q.wait_for("[net] DHCP", timeout=20)
        time.sleep(0.5)

        # 'run evil': an attempt to read kernel memory directly
        q.type_line("clear")
        q.type_line("run evil")
        screen = q.wait_for("process 'evil' killed")
        check("evil read kernel memory and was killed",
              "process 'evil' killed" in screen)
        check("CR2 shows the attacked address",
              "reached for the address 100000" in screen)
        check("the code after the attack never ran",
              "THIS SHOULD NOT BE VISIBLE" not in screen)

        # 'run evil2': the same addresses, but passed to the kernel in
        # syscalls
        q.type_line("run evil2")
        ev2 = q.wait_for("the boundary holds", timeout=20)
        check("the syscalls refuse pointers into kernel memory",
              "the kernel refused every attempt" in ev2)
        check("evil2 did not print kernel memory on the console",
              "IT PASSED" not in ev2 and "A HOLE" not in ev2)
        check("the process survived the refused syscalls (code 0)",
              "code 0" in ev2)
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
