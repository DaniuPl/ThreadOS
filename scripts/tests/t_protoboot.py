#!/usr/bin/env python3
"""ProtonOS smoke test: our own UEFI boot loader (protoboot).

The system is started here by nothing borrowed. On the disk there is an
EFI system partition with three files - our loader, the kernel and the
initrd - and no GRUB anywhere. The firmware runs BOOTX64.EFI, and from
there on everything is our code: reading the files, the graphics mode,
the ACPI pointer, the Multiboot 2 tag table, leaving long mode and the
jump into the kernel.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
LOADER = os.path.join(ROOT, "build", "BOOTX64.EFI")

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"


def pe_says(path):
    """What the file itself says it is: a PE32+ EFI application for
    x86-64. The firmware will not start anything else, so it is worth
    reading it out of the bytes rather than trusting the compiler."""
    with open(path, "rb") as fh:
        head = fh.read(4096)
    if head[:2] != b"MZ":
        return None
    off = struct.unpack_from("<I", head, 0x3C)[0]
    if head[off:off + 4] != b"PE\0\0":
        return None
    machine = struct.unpack_from("<H", head, off + 4)[0]
    magic = struct.unpack_from("<H", head, off + 24)[0]
    subsys = struct.unpack_from("<H", head, off + 24 + 68)[0]
    return machine, magic, subsys


def main():
    print()
    print("--- wlasny program rozruchowy UEFI (protoboot) ---")

    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("POMINIETE: brak obrazu 64-bitowego (make kernel64)")
        return summary()
    if INITRD64 is None or not os.path.exists(INITRD64):
        print("POMINIETE: brak initrd 64-bitowego")
        return summary()
    if not os.path.exists(LOADER):
        # Nobody has built it yet - so we build it, and if there is
        # nothing to build it with, we say so instead of failing
        subprocess.run(["make", "efi"], cwd=ROOT,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not os.path.exists(LOADER):
        print("POMINIETE: brak build/BOOTX64.EFI (apt install clang lld)")
        return summary()
    if not os.path.exists(OVMF_CODE) or not os.path.exists(OVMF_VARS):
        print("POMINIETE: brak firmware OVMF (apt install ovmf)")
        return summary()
    if shutil.which("qemu-system-x86_64") is None:
        print("POMINIETE: brak qemu-system-x86_64")
        return summary()

    said = pe_says(LOADER)
    check("program rozruchowy jest plikiem PE32+ dla x86-64",
          said is not None and said[0] == 0x8664 and said[1] == 0x20b)
    check("i ma podsystem aplikacji EFI (10)",
          said is not None and said[2] == 10)

    esp = tempfile.mkdtemp()
    slog = tempfile.mktemp(suffix=".log")
    nvram = tempfile.mktemp(suffix=".fd")
    try:
        os.makedirs(os.path.join(esp, "EFI", "BOOT"))
        shutil.copy(LOADER, os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI"))
        shutil.copy(KERNEL64, os.path.join(esp, "protonos64.bin"))
        shutil.copy(INITRD64, os.path.join(esp, "initrd64.img"))
        kbytes = os.path.getsize(KERNEL64)
        ibytes = os.path.getsize(INITRD64)

        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, esp=esp, serial=slog,
                 qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            out = q.wait_serial("protoboot: ProtonOS", timeout=60)
            check("firmware uruchamia nasz program rozruchowy",
                  "protoboot: ProtonOS" in out)
            # No GRUB was put on this disk, so anything that starts had
            # better be ours
            check("na tej plycie nie ma zadnego cudzego loadera",
                  "GRUB" not in out and "grub" not in out)

            head = q.wait_serial("ACPI:", timeout=30)
            m = re.search(r"jadro:\s+(\d+) B", head)
            check(f"loader odczytuje jadro z partycji EFI "
                  f"({m.group(1) if m else '?'} B, plik ma {kbytes} B)",
                  m is not None and int(m.group(1)) == kbytes)
            m = re.search(r"initrd:\s+(\d+) B", head)
            check(f"loader odczytuje initrd "
                  f"({m.group(1) if m else '?'} B, plik ma {ibytes} B)",
                  m is not None and int(m.group(1)) == ibytes)
            m = re.search(r"ekran:\s+(\d+)x(\d+), bufor 0x([0-9a-f]+)", head)
            check("loader sam ustawia tryb graficzny przez GOP"
                  + (f" ({m.group(1)}x{m.group(2)})" if m else ""),
                  m is not None and int(m.group(1)) >= 640
                  and int(m.group(2)) >= 400 and int(m.group(3), 16) != 0)
            loader_w = int(m.group(1)) if m else 0
            loader_h = int(m.group(2)) if m else 0
            m = re.search(r"ACPI:\s+0x([0-9a-f]+) \((1\.0|2\.0)\)", head)
            check("loader wyjmuje wskaznik ACPI z tablicy konfiguracyjnej"
                  + (f" ({m.group(2)})" if m else ""),
                  m is not None and int(m.group(1), 16) != 0)

            # ExitBootServices and the way down from long mode: after
            # this line there is no firmware left to complain, so the
            # next word can only come from the kernel
            gone = q.wait_serial("oddaje maszyne jadru", timeout=30)
            check("loader zegna sie z firmware (ExitBootServices)",
                  "protoboot: oddaje maszyne jadru" in gone)

            boot = q.wait_serial("Multiboot OK", timeout=60)
            check("jadro rusza po zejsciu z trybu dlugiego i powrocie do niego",
                  "Jadro uruchomione w trybie dlugim" in boot)
            check("jadro rozpoznaje znaczniki Multiboot 2 od naszego loadera",
                  "Multiboot OK (Multiboot 2)" in boot)

            # Three modules: the initrd, which the kernel has always
            # taken by position, and behind it the kernel image and the
            # loader itself - handed over by name, because a system
            # that may be asked to install itself has to have the
            # FILES, and the kernel in memory is loaded segments
            mods = q.wait_serial("Moduly od loadera:", timeout=30)
            check("initrd, obraz jadra i sam loader docieraja jako moduly",
                  "Moduly od loadera: 3" in mods)

            fb = q.wait_serial("Bufor obrazu:", timeout=30)
            m = re.search(r"Bufor obrazu: (\d+)x(\d+), (\d+) bitow", fb)
            check(f"jadro dostaje ten sam bufor obrazu, ktory loader ustawil "
                  f"({loader_w}x{loader_h})",
                  m is not None and int(m.group(1)) == loader_w
                  and int(m.group(2)) == loader_h)

            acpi = q.wait_serial("Wskaznik ACPI", timeout=30)
            check("wskaznik ACPI przechodzi przez znacznik do jadra",
                  re.search(r"Wskaznik ACPI od loadera: [0-9a-f]+",
                            acpi) is not None)

            mem = q.wait_serial("Pamiec fizyczna:", timeout=30)
            m = re.search(r"Pamiec fizyczna: (\d+) KiB RAM", mem)
            check(f"mapa pamieci UEFI przelozona na Multiboot 2 ma sens"
                  + (f" ({m.group(1)} KiB)" if m else ""),
                  m is not None and int(m.group(1)) > 64 * 1024)

            # The sharpest check of the memory map: the kernel and the
            # initrd sit in memory the loader took for itself. If those
            # pages went into the map as free, the allocator would hand
            # them out and the program from the initrd would be rubbish
            # by the time anyone ran it.
            elf = q.wait_serial("ELF64 z initrd:", timeout=60)
            check("program z initrd dziala - loader nie oddal wlasnej pamieci",
                  "pierscien 3" in elf and "kod wyjscia 7" in elf)

            q.wait_serial("System 64-bitowy gotowy", timeout=60)
            w, h, at = q.screen_raw()
            check(f"rozdzielczosc ekranu zgadza sie z ta od loadera ({w}x{h})",
                  w == loader_w and h == loader_h)
            x0, y0 = (w - 640) // 2, (h - 200) // 2
            lit = sum(1 for y in range(y0, y0 + 200)
                      for x in range(x0, x0 + 640)
                      if at(x, y) != (0, 0, 0))
            check(f"konsola rysuje sie na ekranie od naszego loadera "
                  f"({lit} pikseli)", lit > 2000)
        finally:
            q.close()
    finally:
        shutil.rmtree(esp, ignore_errors=True)
        for f in (slog, nvram):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
