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
        q.wait_for("[siec] DHCP", timeout=20)
        time.sleep(0.5)

        q.type_line("clear")
        q.type_line("mem")
        used_before_sg = pmm_used(q.screen())
        q.type_line("run sigtest")
        screen = q.wait_for("[sigtest] wlasna procedura obslugi", timeout=30)
        check("wlasna procedura obslugi sygnalu dziala w ring 3 i wraca",
              "dostala SIGINT i wrocila" in screen)
        screen = q.wait_for("[sigtest] SIG_IGN", timeout=20)
        check("SIG_IGN gubi sygnal zamiast go doreczac",
              "sygnal przepadl" in screen)
        screen = q.wait_for("[sigtest] rodzic dostal SIGCHLD", timeout=30)
        check("SIGTERM domyslnie konczy proces (kod -15)",
              "domyslnie konczy proces (kod -15)" in screen)
        check("rodzic dostaje SIGCHLD po smierci dziecka",
              "[sigtest] rodzic dostal SIGCHLD" in screen)
        screen = q.wait_for("[sigtest] SIGKILL nie do przechwycenia",
                            timeout=30)
        check("SIGKILL-a nie da sie przechwycic ani zignorowac",
              "[sigtest] SIGKILL nie do przechwycenia" in screen)

        # Ctrl+C from a real keyboard -> a SIGINT to the process holding
        # the console; the wait for a key is interrupted halfway
        q.wait_for("[sigtest] czekam na Ctrl+C", timeout=20)
        q.sendkey("ctrl-c")
        screen = q.wait_for("[sigtest] Ctrl+C dotarl", timeout=20)
        check("Ctrl+C dociera do procesu jako SIGINT i przerywa odczyt",
              "dotarl jako SIGINT" in screen)
        q.wait_for("proces 'sigtest' zakonczyl sie (kod 0)", timeout=20)
        time.sleep(0.6)
        q.type_line("mem")
        used_after_sg = pmm_used(q.screen())
        check(f"brak wycieku ramek po sygnalach"
              f" ({used_before_sg} -> {used_after_sg})",
              used_after_sg is not None and used_after_sg == used_before_sg)

        # The kernel's shell can send any signal, by a process's name too
        q.type_line("clear")
        q.type_line("run spin")
        q.wait_for("[spin] tik", timeout=20)
        q.type_line("kill spin 15")
        screen = q.wait_for("zakonczony przez SIGTERM", timeout=20)
        check("'kill nazwa 15' z shella jadra wysyla SIGTERM",
              "kill: SIGTERM -> pid" in screen)
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
