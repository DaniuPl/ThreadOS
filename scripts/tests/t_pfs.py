#!/usr/bin/env python3
"""ProtonOS smoke test: the PFS file system on the disk.

A file written by a ring 3 process and a file written from the kernel's
shell are to be the same file seen by both - and both are to survive a
restart of the machine. That last part matters most here: everything
else can be faked with a cache, and a SECOND BOOT on the same disk image
cannot.

At the end of the first boot we call 'crash', that is, a deliberate page
fault in the kernel. The diagnostic is to give the exception number and
the address from CR2, and the system is to stop rather than blunder on.

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

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        q.wait_for("ProtonOS shell", timeout=40)

        # --- The file system on the disk (PFS) ---
        # A ring 3 process writes and reads a file with syscalls
        q.type_line("run fileio")
        screen = q.wait_for("[fileio] OK")
        check("proces ring3 zapisal i odczytal plik z dysku",
              "odczytano z dysku: plik zapisany przez proces ring3" in screen)

        # The kernel's shell: write -> cat (a write and a read within one boot)
        q.type_line("write greeting witaj-z-dysku-pfs")
        check("zapis pliku z shella jadra",
              "zapisano" in q.wait_for("zapisano"))
        q.type_line("cat greeting")
        check("odczyt zapisanego pliku (cat)",
              "witaj-z-dysku-pfs" in q.wait_for("witaj-z-dysku-pfs"))

        # fls shows both files
        q.type_line("fls")
        screen = q.wait_for("Katalog glowny")
        check("fls listuje pliki na dysku",
              "greeting" in screen and "ring3.txt" in screen)

        # cat of a file that does not exist
        q.type_line("cat niema")
        check("cat zglasza brak pliku",
              "brak pliku 'niema'" in q.wait_for("brak pliku"))

        # The 'cat' program in ring 3: it reads a file through an fd and
        # writes to fd 1 (a unified I/O model: read the file -> write to
        # the console). We clear the screen so as not to catch the old
        # echo of 'witaj-z-dysku-pfs'.
        q.type_line("clear")
        time.sleep(0.3)
        q.type_line("run cat greeting")
        check("program ring3 'cat' czyta plik przez fd i pisze na fd 1",
              "witaj-z-dysku-pfs" in q.wait_for("witaj-z-dysku-pfs",
                                                timeout=20))

        # At the end: crash should cause a page fault with a diagnostic
        q.type_line("crash")
        screen = q.screen()
        check("page fault obsluzony (wyjatek 14)",
              "WYJATEK CPU 14: Page Fault" in screen)
        check("CR2 wskazuje felerny adres",
              "(CR2): e0000000" in screen)
        check("system zatrzymany po wyjatku", "System zatrzymany" in screen)
    finally:
        q.close()

    # --- Persistence: a second boot on THE SAME disk ---
    print("--- drugi boot (ten sam dysk) ---")
    q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        screen = q2.wait_for("ProtonOS shell")
        check("drugi boot: dysk wykryty"
              " (juz sformatowany, bez ponownego format)",
              "+ PFS gotowy" in screen and "sformatowany" not in screen)

        q2.type_line("fls")
        screen = q2.wait_for("Katalog glowny")
        check("pliki przetrwaly restart (fls)",
              "greeting" in screen and "ring3.txt" in screen)

        q2.type_line("cat greeting")
        check("zawartosc pliku przetrwala restart (cat)",
              "witaj-z-dysku-pfs" in q2.wait_for("witaj-z-dysku-pfs"))

        q2.type_line("cat ring3.txt")
        check("plik zapisany przez ring3 przetrwal restart",
              "plik zapisany przez proces ring3" in q2.wait_for("ring3"))
    finally:
        q2.close()
        if os.path.exists(disk):
            os.unlink(disk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
