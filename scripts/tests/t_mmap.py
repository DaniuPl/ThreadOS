#!/usr/bin/env python3
"""ProtonOS smoke test: files mapped into memory.

The ELF loader and the file system we joined earlier - a program can lie
on the disk. This is the other half of the same thought: a file can be
MEMORY. The program gets an address and reads the contents like an
ordinary array, without a single read call.

We check three things, each a different way:

  1. the bytes at the address match what lies in the file,
  2. the pages arrive UNDER YOUR FINGERS - touching one byte of a
     hundred-page file is to cost one page, not a hundred,
  3. the mapping is read-only and an attempted write is to kill the
     process rather than quietly vanish.

The kernel works the block numbers out once, when the mapping is set up,
and reads them afterwards through the block layer, bypassing the PFS
code - for the same reason as with page swapping: PFS has one shared
buffer for a block, and a page fault can land in the middle of any file
operation.

Both kernels have the same thing, each with its own paging code - and we
check both the same way. Along the way we check here the other area that
grows under your fingers: the stack. A program gets eight pages, and a
recursion goes considerably deeper.
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


def pages_from_files(text):
    """How many pages the kernel read from mapped files - the shell gives
    that in the summary after every program."""
    m = re.search(r"z plikow (\d+)\)", text)
    return int(m.group(1)) if m else None


def main():
    print("--- pliki odwzorowane w pamieci (jadro 64-bitowe) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return 0

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

            # --- A small file: read whole through a mapping ---
            make_it = q.command("run mkmap64 mapa.txt 12", "kod wyjscia",
                               timeout=60)
            check("plik z wzorcem zalozony na dysku",
                  "[mkmap64] zapisano 12288 B wzorca" in make_it)

            read_out = q.command("run mmapfile64 mapa.txt", "kod wyjscia",
                               timeout=90)
            addr = re.search(r"plik pod adresem (\d+)", read_out)
            check("plik dostal adres w obszarze mapowan",
                  addr is not None and
                  0x50000000 <= int(addr.group(1)) < 0x60000000)
            check("wszystkie bajty pliku zgadzaja sie z wzorcem",
                  "sprawdzone bajty zgadzaja sie, bylo ich 12288" in read_out)
            check("proba zapisu do odwzorowania zabija proces",
                  "zapis do odwzorowania odrzucony" in read_out and
                  "zabite: Page Fault" in read_out)
            check("tresc pliku po odrzuconym zapisie nietknieta",
                  "ZLE" not in read_out)
            check("plik przeczytany bez ani jednego wywolania read",
                  "bez ani jednego read" in read_out)
            check("dwanascie kilobajtow to dokladnie trzy strony z dysku",
                  pages_from_files(read_out) == 3)

            # --- A large file, one byte touched ---
            # This is the real proof of paging on demand: a mapping
            # costs as much as was taken from it
            q.command("run mkmap64 duza.txt 400", "kod wyjscia",
                        timeout=120)
            malo = q.command("run mmapfile64 duza.txt 1", "kod wyjscia",
                               timeout=90)
            check("odwzorowanie duzego pliku sie udaje",
                  "reszty pliku nie tknalem" in malo)
            check("dotkniecie jednego bajtu kosztuje jedna strone, "
                  "a nie caly plik", pages_from_files(malo) == 1)

            # ...and reading the whole of it costs as much as it has pages
            many = q.command("run mmapfile64 duza.txt", "kod wyjscia",
                               timeout=120)
            check("przeczytanie calego pliku wciaga wszystkie jego strony",
                  pages_from_files(many) == 400 * 1024 // 4096)

            # A mapping does not outlive the process: the memory returns to the
# pool
            m = re.search(r"mmapfile64\] koniec, kod wyjscia 0 "
                          r"\(ramek (\d+) -> (\d+)", many)
            check("po koncu programu ramki wrocily do puli",
                  m is not None and m.group(1) == m.group(2))

            # A file that does not exist is an error, not an empty address
            missing = q.command("run mmapfile64 niczegotakiego",
                               "kod wyjscia", timeout=60)
            check("nieistniejacy plik zglaszany jako blad",
                  "nie da sie odwzorowac pliku" in missing)
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, disk):
            if os.path.exists(f):
                os.unlink(f)

    # --- The same in the 32-bit kernel ---
    print("--- pliki odwzorowane w pamieci (jadro 32-bitowe) ---")
    disk32 = tempfile.mktemp(suffix=".img")
    slog32 = tempfile.mktemp(suffix=".log")
    with open(disk32, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        q3 = Qemu(KERNEL, INITRD, disk32, append=TEXT_MODE, serial=slog32)
        try:
            q3.wait_for("ProtonOS shell", timeout=40)
            q3.wait_for("[siec] DHCP", timeout=30)

            make_it = q3.command("run mkmap mapa.txt 12", "zakonczyl sie",
                                timeout=60)
            check("plik z wzorcem zalozony na dysku (32-bit)",
                  "[mkmap] zapisano 12288 B wzorca" in make_it)

            read_out = q3.command("run mmapfile mapa.txt", "zakonczyl sie",
                                timeout=90)
            addr = re.search(r"plik pod adresem (\d+)", read_out)
            check("plik dostal adres w obszarze mmap (32-bit)",
                  addr is not None and
                  0xB0000000 <= int(addr.group(1)) < 0xBF000000)
            check("wszystkie bajty pliku zgadzaja sie z wzorcem (32-bit)",
                  "sprawdzone bajty zgadzaja sie, bylo ich 12288" in read_out)
            check("proba zapisu do odwzorowania zabija proces (32-bit)",
                  "zapis do odwzorowania odrzucony" in read_out and
                  re.search(r"ubity na rdzeniu \d+: Page Fault", read_out) is not None)
            check("tresc pliku po odrzuconym zapisie nietknieta (32-bit)",
                  "ZLE" not in read_out)

            # Paging on demand: a large file, one byte touched
            before = q3.command("mem", "z plikow:")
            m1 = re.search(r"z plikow:\s+(\d+)", before)
            q3.command("run mkmap duza.txt 400", "zakonczyl sie",
                         timeout=120)
            malo = q3.command("run mmapfile duza.txt 1", "zakonczyl sie",
                                timeout=90)
            check("odwzorowanie duzego pliku sie udaje (32-bit)",
                  "reszty pliku nie tknalem" in malo)
            po = q3.command("mem", "z plikow:")
            m2 = re.search(r"z plikow:\s+(\d+)", po)
            check("dotkniecie jednego bajtu kosztuje jedna strone (32-bit)",
                  m1 is not None and m2 is not None and
                  int(m2.group(1)) - int(m1.group(1)) == 1)

            # The stack grows under your fingers too. A program gets eight
            # pages, and a recursion goes far deeper - and on the way
            # back it checks whether every frame is still its own.
            # Adding pages on top of overwriting somebody else's frames
            # would look the same halfway.
            stack = q3.command("run deep", "zakonczyl sie", timeout=60)
            check("stos rosnie na zadanie: rekurencja siega poza osiem "
                  "stron", "[deep] zeszlem na poziom 40" in stack)
            check("i kazda ramka przetrwala nietknieta",
                  "wszystkie ramki caly" in stack and "ZLE" not in stack)
        finally:
            q3.close()
    finally:
        for f in (disk32, slog32):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
