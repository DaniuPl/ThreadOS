#!/usr/bin/env python3
"""ProtonOS smoke test: an NVMe disk (PCI Express).

The block layer has existed since the days of AHCI: the file system and
page swapping do not know which controller sits underneath. NVMe is the
third driver under that same name and the least like the rest - there is
nothing of the ATA heritage in it. Instead of registers and FIS frames
there are two queues in memory: we put commands in, the controller puts
answers back, and a phase bit says that something has returned.

We check that the way a disk is checked: we write, we read, and then
start the machine a SECOND TIME on the same image. A file that survived
a restart really did lie on the platter and not in memory.
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
    # --- The 32-bit kernel ---
    print("--- dysk NVMe (jadro 32-bitowe) ---")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    slog = tempfile.mktemp(suffix=".log")

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog, nvme=True)
    try:
        start = q.wait_for("+ PFS gotowy", timeout=60)
        check("warstwa blokowa wybrala NVMe zamiast ATA",
              "Dysk NVMe (PCIe, kolejki) + PFS gotowy" in start)
        check("system plikow sformatowal pusty dysk NVMe",
              "sformatowany" in start)
        # The shell starts only after the scheduler, and keys typed
        # earlier are lost - that is not a thing to guess at
        q.wait_for("ProtonOS shell", timeout=30)
        q.wait_for("[siec] DHCP", timeout=30)

        # Page swapping creates its file only after the file system,
        # so we ask about it only now
        check("plik wymiany zalozony na dysku NVMe",
              re.search(r"Wymiana stron: \d+ KiB",
                        q.wait_serial("Wymiana stron:", timeout=20))
              is not None)

        write_out = q.command("write nvmetest tresc-przez-kolejki", "zapisano")
        check("zapis pliku na NVMe", "zapisano" in write_out)
        read_res = q.command("cat nvmetest", "tresc-przez-kolejki")
        check("odczyt tego samego pliku oddaje te same bajty",
              "tresc-przez-kolejki" in read_res)

        # A ring 3 process by the same path: syscall -> PFS -> NVMe
        ring3 = q.command("run fileio", "[fileio] OK", timeout=40)
        check("proces ring3 pisze i czyta plik przez NVMe",
              "odczytano z dysku: plik zapisany przez proces ring3" in ring3)

        # A large file: a dozen or so batches of one page each - here
        # it can be seen whether the loop splitting the transfer counts
        # correctly
        big = q.command("run memtest", "[memtest]", timeout=60)
        check("system zyje po intensywnym ruchu na dysku",
              "[memtest]" in big)
    finally:
        q.close()
        if os.path.exists(slog):
            os.unlink(slog)

    # --- A second boot on the same image ---
    print("--- NVMe: drugi rozruch, ten sam obraz ---")
    slog2 = tempfile.mktemp(suffix=".log")
    q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog2, nvme=True)
    try:
        start = q2.wait_for("+ PFS gotowy", timeout=60)
        check("drugi rozruch: dysk juz sformatowany",
              "sformatowany" not in start)
        q2.wait_for("ProtonOS shell", timeout=30)
        q2.wait_for("[siec] DHCP", timeout=30)
        read_res = q2.command("cat nvmetest", "tresc-przez-kolejki")
        check("plik przetrwal restart - naprawde lezal na dysku",
              "tresc-przez-kolejki" in read_res)
        listing = q2.command("fls", "ring3.txt")
        check("plik zapisany przez ring3 tez przetrwal",
              "ring3.txt" in listing)
    finally:
        q2.close()
        for f in (disk, slog2):
            if os.path.exists(f):
                os.unlink(f)

    # --- The 64-bit kernel ---
    print("--- dysk NVMe (jadro 64-bitowe) ---")
    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog64 = tempfile.mktemp(suffix=".log")
    disk64 = tempfile.mktemp(suffix=".img")
    with open(disk64, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    try:
        check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        q64 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog64, disk=disk64,
                   nvme=True, qemu="qemu-system-x86_64")
        try:
            # We wait for the WHOLE line, not for the "PFS" tag alone:
            # that appears already when the disk is formatted, so waiting
            # for it was a race with the driver's message
            start = q64.wait_serial("+ PFS gotowy", timeout=90)
            check("jadro 64-bitowe tez widzi dysk NVMe",
                  "Dysk NVMe (PCIe, kolejki) + PFS gotowy" in start)
            q64.wait_serial("proton64>", timeout=30)

            q64.command("write plik64 tresc-z-trybu-dlugiego", "write:")
            read_res = q64.command("cat plik64", "tresc-z-trybu-dlugiego")
            check("zapis i odczyt przez NVMe w trybie dlugim",
                  "tresc-z-trybu-dlugiego" in read_res)

            # A program from the NVMe disk: the ELF loader reads it by the
            # same path
            q64.command("cp hello64 znvme", "cp:")
            run_out = q64.command("run znvme", "[hello64] koncze",
                                  timeout=40)
            check("program uruchomiony wprost z dysku NVMe",
                  "Program 'znvme' z dysku" in run_out)
        finally:
            q64.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog64, disk64):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
