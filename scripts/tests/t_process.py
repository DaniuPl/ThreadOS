#!/usr/bin/env python3
"""ProtonOS smoke test: ring 3 processes and their memory.

A user program is a separate ELF file with its own address space. This
group walks in turn through everything such a program gets from the
kernel: system calls, arguments, a growing heap, mmap, shared memory,
threads and priorities.

After EVERY program we ask 'mem' for the number of frames taken and
compare it with the one from before the start. A process that has
finished is to give everything back - that is the only assertion that
catches a memory leak, because the program itself will say nothing about
it.

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

        q.type_line("mem")
        used_before = pmm_used(q.screen())

        # 'run hello': an ELF process with its own address space
        q.type_line("run hello")
        screen = q.wait_for("[hello] czesc, jestem procesem ELF")
        check("proces hello wystartowal z ELF i robi syscall write",
              "[hello] czesc, jestem procesem ELF" in screen)
        # User space starts from 1 GiB - below there is only the kernel's
        # identity mapping
        check("program uzytkownika mieszka powyzej 1 GiB",
              re.search(r"entry 4[0-9a-f]{7}", screen) is not None)
        view = q.wait_for("[hello] licze: 3")
        check("syscalle write+sleep dzialaja w petli",
              "[hello] licze: 3" in view)
        view = q.wait_for("proces 'hello' zakonczyl sie (kod 0)")
        check("hello zakonczyl sie czysto przez sys_exit",
              "proces 'hello' zakonczyl sie (kod 0)" in view)

        time.sleep(0.5)  # time to clean the zombie up
        q.type_line("mem")
        used_after = pmm_used(q.screen())
        check(f"brak wycieku ramek po procesie ({used_before} -> {used_after})",
              used_before is not None and used_after == used_before)

        # 'run echo ...': the arguments (argc/argv) reach the process
        q.type_line("run echo proton os dziala")
        check("argumenty argc/argv docieraja do procesu (echo)",
              "proton os dziala" in q.wait_for("proton os dziala"))

        # 'run memtest': sbrk/malloc gives the process growing memory
        q.type_line("run memtest")
        screen = q.wait_for("[memtest] OK:")
        check("sbrk/malloc dziala (memtest zapisal i zweryfikowal pamiec)",
              "blokow po" in screen and "zweryfikowanych" in screen)
        check("memtest nie zglosil bledu pamieci",
              "BLAD" not in screen.split("[memtest] OK")[0]
              .split("[memtest] start")[-1])

        time.sleep(0.5)
        q.type_line("mem")
        used_after_mt = pmm_used(q.screen())
        check(f"brak wycieku ramek po memtest"
              f" ({used_before} -> {used_after_mt})",
              used_after_mt is not None and used_after_mt == used_before)

        # 'run mmaptest': mmap gives fresh, zeroed, writable memory
        q.type_line("run mmaptest")
        check("mmap dziala (3 strony zmapowane, zerowe i zapisywalne)",
              "zapisane i zweryfikowane" in q.wait_for("[mmaptest] OK:"))
        time.sleep(0.5)
        q.type_line("mem")
        used_after_mm = pmm_used(q.screen())
        check(f"brak wycieku ramek po mmaptest"
              f" ({used_before} -> {used_after_mm})",
              used_after_mm is not None and used_after_mm == used_before)

        # 'run shmtest': shared memory between processes. shmtest writes
        # 12345 and starts shmread (a separate process), which reads the
        # same memory - the proof is its line with the value read.
        q.type_line("run shmtest")
        check("pamiec wspoldzielona: inny proces czyta zapisana wartosc",
              "[shmread] wartosc=12345" in q.wait_for("[shmread] wartosc="))

        # 'run threads': two threads in one process (shared memory). Each
        # writes a different global variable; main adds them up -> 33.
        # A fresh frame baseline (shmtest took a permanent shared region).
        q.type_line("mem")
        used_before_th = pmm_used(q.screen())
        q.type_line("run threads")
        check("watki w procesie dziela pamiec (suma globalnych = 33)",
              "suma=33" in q.wait_for("[threads] watki zapisaly"))
        # no frame leak after a multi-threaded process (the threads' stacks
        # + the address space freed when the last thread ends)
        time.sleep(0.5)
        q.type_line("mem")
        used_after_th = pmm_used(q.screen())
        check(f"brak wycieku ramek po watkach"
              f" ({used_before_th} -> {used_after_th})",
              used_after_th is not None and used_after_th == used_before_th)

        # 'run prio': the scheduler with priorities. The normal thread
        # preempts the low one -> over the same time window it will count
        # many times more.
        q.type_line("run prio")
        screen = q.wait_for("[prio] normalny=", timeout=20)
        m = re.search(r"normalny=(\d+) niski=(\d+)", screen)
        check("scheduler z priorytetami: normalny >> niski"
              + (f" ({m.group(1)} vs {m.group(2)})" if m else ""),
              m is not None and int(m.group(1)) > int(m.group(2)) * 5)

        # The cryptography under TLS: the RFC vectors computed in ring 3
        q.type_line("clear")
        q.type_line("run cryptotest")
        crypto = q.wait_for("[crypto] wszystkie", timeout=20)
        check("wektory SHA-256/HMAC/HKDF/ChaCha20/Poly1305/X25519 z RFC",
              "wszystkie wektory zgadzaja sie" in crypto
              and "BLAD" not in crypto)
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
