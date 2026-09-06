#!/usr/bin/env python3
"""ProtonOS smoke test: signals.

There is one sharpest point here: the return from a handler. The signal
arrives exactly at the moment the kernel has already put down the result
of the interrupted system call, and the handler runs in ring 3 on the
program's stack - and has to return in such a way that the program
notices nothing.

Ctrl+C goes through a real keyboard, not through a shortcut inside the
kernel: the key is to reach the process holding the console as a SIGINT
and interrupt its wait for a character halfway.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
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
        q.wait_for("[net] DHCP", timeout=20)
        time.sleep(0.5)

        q.type_line("clear")
        q.type_line("mem")
        used_before_sg = pmm_used(q.screen())
        q.type_line("run sigtest")
        screen = q.wait_for("[sigtest] my own handler", timeout=30)
        check("a handler of one's own runs in ring 3 and comes back",
              "got SIGINT and came back" in screen)
        screen = q.wait_for("[sigtest] SIG_IGN", timeout=20)
        check("SIG_IGN loses the signal instead of delivering it",
              "the signal was lost" in screen)
        screen = q.wait_for("[sigtest] the parent got SIGCHLD", timeout=30)
        check("SIGTERM ends a process by default (code -15)",
              "ends a process by default (code -15)" in screen)
        check("the parent gets SIGCHLD after the child dies",
              "[sigtest] the parent got SIGCHLD" in screen)
        screen = q.wait_for("[sigtest] SIGKILL cannot be caught",
                            timeout=30)
        check("SIGKILL can be neither caught nor ignored",
              "[sigtest] SIGKILL cannot be caught" in screen)

        # Ctrl+C from a real keyboard -> a SIGINT to the process holding
        # the console; the wait for a key is interrupted halfway
        q.wait_for("[sigtest] waiting for Ctrl+C", timeout=20)
        q.sendkey("ctrl-c")
        screen = q.wait_for("[sigtest] Ctrl+C arrived", timeout=20)
        check("Ctrl+C reaches the process as SIGINT and interrupts the read",
              "arrived as SIGINT" in screen)
        q.wait_for("process \'sigtest\' has ended (code 0)", timeout=20)
        time.sleep(0.6)
        q.type_line("mem")
        used_after_sg = pmm_used(q.screen())
        check(f"no frames leaked after the signals"
              f" ({used_before_sg} -> {used_after_sg})",
              used_after_sg is not None and used_after_sg == used_before_sg)

        # The kernel's shell can send any signal, by a process's name too
        q.type_line("clear")
        q.type_line("run spin")
        q.wait_for("[spin] tick", timeout=20)
        q.type_line("kill spin 15")
        screen = q.wait_for("ended by SIGTERM", timeout=20)
        check("'kill name 15' from the kernel's shell sends SIGTERM",
              "kill: SIGTERM -> pid" in screen)
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
