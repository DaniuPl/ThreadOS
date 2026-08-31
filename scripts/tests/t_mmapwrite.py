#!/usr/bin/env python3
"""ProtonOS smoke test: writing through a file mapped into memory.

Mappings were one-way so far: a file could be read like memory, but not
written. Now the road goes both ways - the program changes bytes at an
address and they land in the file on disk.

Along the way we check four things that are easy to get wrong:

  1. We flush ONLY what changed. The program reads the whole file and
     writes over two pages - and two pages are to go back to the disk.
     Which ones is said by the processor: it sets the "dirty" bit itself
     on a write. A second sys_msync in a row is to flush zero.
  2. The changes can be seen with an ORDINARY read, and after a machine
     restart on the disk too - not only in the memory that held them.
  3. The program does not have to ask: what it did not manage to flush
     itself is flushed by the kernel when the process ends.
  4. A writable mapping after a fork is SHARED, not copied on write. Both
     processes write into one file, so a private copy in the child would
     mean its writes never go beyond its own memory.

Both kernels have the same thing, each with its own paging code - and we
check both the same way.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def main():
    print("--- zapis przez odwzorowanie (jadro 64-bitowe) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
    else:
        d = tempfile.mkdtemp()
        iso = tempfile.mktemp(suffix=".iso")
        slog = tempfile.mktemp(suffix=".log")
        disk = tempfile.mktemp(suffix=".img")
        with open(disk, "wb") as fh:
            fh.truncate(DISK_SIZE)
        try:
            check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
            if not os.path.exists(iso):
                return summary()

            q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                     sata=True, qemu="qemu-system-x86_64")
            try:
                q.wait_serial("proton64>", timeout=90)
                q.wait_serial("PFS gotowy", timeout=30)

                make_it = q.command("run mkmap64 zapis.txt 12", "kod wyjscia",
                                   timeout=60)
                check("plik z wzorcem zalozony na dysku",
                      "[mkmap64] zapisano 12288 B wzorca" in make_it)

                write_it = q.command("run mmapzap64 zapis.txt write",
                                   "kod wyjscia", timeout=90)
                check("caly plik da sie przeczytac przez odwzorowanie "
                      "do zapisu",
                      "stary wzorzec przeczytany, bajtow 12288" in write_it)
                check("odsylane sa tylko strony, do ktorych pisano",
                      "odeslanych stron: 2" in write_it)
                check("odeslana strona przestaje byc brudna",
                      "drugie msync odeslalo: 0" in write_it)
                check("odeslanie na dysk nie rusza tresci w pamieci",
                      "zapis przez pamiec gotowy" in write_it and "ZLE" not in write_it)

                probe = q.command("run mmapzap64 zapis.txt check",
                                  "kod wyjscia", timeout=90)
                check("zwykly odczyt widzi to, co wpisala pamiec",
                      "zwykly odczyt widzi to" in probe and "ZLE" not in probe)

                # Without an explicit msync - the kernel flushes it at the end of the
# process
                q.command("run mkmap64 bezsync.txt 12", "kod wyjscia",
                            timeout=60)
                without = q.command("run mmapzap64 bezsync.txt nosync",
                                  "kod wyjscia", timeout=60)
                check("program moze skonczyc sie bez proszenia o odeslanie",
                      "koncze bez msync" in without)
                probe2 = q.command("run mmapzap64 bezsync.txt check2",
                                   "kod wyjscia", timeout=60)
                check("jadro odsyla brudne strony przy koncu procesu",
                      "doszly na dysk przy koncu procesu" in probe2)

                # After a fork the mapping is shared, not copied
                q.command("run mkmap64 wspolny.txt 12", "kod wyjscia",
                            timeout=60)
                visible = q.command("run mmapzap64 wspolny.txt shared",
                                    "kod wyjscia", timeout=60)
                check("rodzic widzi zapis dziecka - odwzorowanie wspolne",
                      "odwzorowanie wspolne" in visible and "ZLE" not in visible)
            finally:
                q.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)
            for f in (iso, slog, disk):
                if os.path.exists(f):
                    os.unlink(f)

    # --- The same in the 32-bit kernel, with a restart at the end ---
    print("--- zapis przez odwzorowanie (jadro 32-bitowe) ---")
    disk32 = tempfile.mktemp(suffix=".img")
    slog32 = tempfile.mktemp(suffix=".log")
    slog4 = tempfile.mktemp(suffix=".log")   # the second boot writes to its own
    with open(disk32, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        q3 = Qemu(KERNEL, INITRD, disk32, append=TEXT_MODE, serial=slog32)
        try:
            q3.wait_for("ProtonOS shell", timeout=40)
            q3.wait_for("[siec] DHCP", timeout=30)

            make_it = q3.command("run mkmap zapis.txt 12", "zakonczyl sie",
                                timeout=60)
            check("plik z wzorcem zalozony na dysku (32-bit)",
                  "[mkmap] zapisano 12288 B wzorca" in make_it)

            write_it = q3.command("run mmapzap zapis.txt write", "zakonczyl sie",
                                timeout=90)
            check("caly plik da sie przeczytac przez odwzorowanie "
                  "do zapisu (32-bit)",
                  "stary wzorzec przeczytany, bajtow 12288" in write_it)
            check("odsylane sa tylko strony, do ktorych pisano (32-bit)",
                  "odeslanych stron: 2" in write_it)
            check("odeslana strona przestaje byc brudna (32-bit)",
                  "drugie msync odeslalo: 0" in write_it)
            check("odeslanie na dysk nie rusza tresci w pamieci (32-bit)",
                  "zapis przez pamiec gotowy" in write_it and "ZLE" not in write_it)

            probe = q3.command("run mmapzap zapis.txt check", "zakonczyl sie",
                               timeout=90)
            check("zwykly odczyt widzi to, co wpisala pamiec (32-bit)",
                  "zwykly odczyt widzi to" in probe and "ZLE" not in probe)

            q3.command("run mkmap bezsync.txt 12", "zakonczyl sie",
                         timeout=60)
            without = q3.command("run mmapzap bezsync.txt nosync", "zakonczyl sie",
                               timeout=60)
            check("program moze skonczyc sie bez proszenia o odeslanie "
                  "(32-bit)", "koncze bez msync" in without)
            probe2 = q3.command("run mmapzap bezsync.txt check2",
                                "zakonczyl sie", timeout=60)
            check("jadro odsyla brudne strony przy koncu procesu (32-bit)",
                  "doszly na dysk przy koncu procesu" in probe2)

            q3.command("run mkmap wspolny.txt 12", "zakonczyl sie",
                         timeout=60)
            visible = q3.command("run mmapzap wspolny.txt shared",
                                 "zakonczyl sie", timeout=60)
            check("rodzic widzi zapis dziecka - odwzorowanie wspolne "
                  "(32-bit)",
                  "odwzorowanie wspolne" in visible and "ZLE" not in visible)
        finally:
            q3.close()

        # The strongest proof: THE SAME disk image, a new kernel, new
        # memory. If the changes sat only in the process's pages, there
        # would be no trace of them here.
        q4 = Qemu(KERNEL, INITRD, disk32, append=TEXT_MODE, serial=slog4)
        try:
            screen_text = q4.wait_for("ProtonOS shell", timeout=40)
            check("drugi rozruch na tym samym dysku (bez formatowania)",
                  "+ PFS gotowy" in screen_text and "sformatowany" not in screen_text)
            q4.wait_for("[siec] DHCP", timeout=30)

            po = q4.command("run mmapzap zapis.txt check", "zakonczyl sie",
                              timeout=90)
            check("zapisy przez pamiec przetrwaly restart maszyny",
                  "zwykly odczyt widzi to" in po and "ZLE" not in po)
            po2 = q4.command("run mmapzap bezsync.txt check2",
                               "zakonczyl sie", timeout=60)
            check("bajty odeslane przy koncu procesu tez przetrwaly restart",
                  "doszly na dysk przy koncu procesu" in po2)
        finally:
            q4.close()
    finally:
        for f in (disk32, slog32, slog4):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
