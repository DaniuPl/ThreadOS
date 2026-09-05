#!/usr/bin/env python3
"""ProtonOS smoke test: FAT12/16/32 - the filesystem of the EFI partition.

The one filesystem here that is not ours, and the reason it exists at
all is that firmware reads FAT and reads nothing else. So every image
in this group is made by mtools and everything the system writes into
one is read back by mtools: an independent implementation on the other
side of every assertion. A driver that agrees only with itself has
proved nothing - least of all on a partition belonging to the system
that boots the machine.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, make_iso64, no64, make_gpt_disk,
                   gpt_intact, neighbours_intact, mtools_ok, fat_format,
                   fat_mkdirs, fat_put, fat_get, fat_dir, fat_kind,
                   make_mbr_fat_disk)

ESP_START = 2048                     # make_gpt_disk puts the ESP there
ESP_SECTORS = 204800                 # 100 MiB

LONG_TEXT = b"To jest plik z dluga nazwa na partycji EFI.\n"
BIG = bytes((i * 7 + 3) & 0xFF for i in range(200000))


def rot_sum(data):
    """The same number the kernel's 'fatsum' computes: every byte
    added into a value rotated left by one. Written out here rather
    than imported, so that the two sides are two implementations."""
    s = 0
    for b in data:
        s = (((s << 1) & 0xFFFFFFFF) | (s >> 31)) + b
        s &= 0xFFFFFFFF
    return s


def main():
    print()
    print("--- FAT12/16/32: partycja EFI ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()
    if not mtools_ok():
        print("POMINIETE: brak mtools (obrazow FAT nie ma czym zrobic)")
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    disk = tempfile.mktemp(suffix=".img")
    small = tempfile.mktemp(suffix=".img")
    tiny = tempfile.mktemp(suffix=".img")
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")

    try:
        check("plyta z jadrem 64-bitowym buduje sie",
              make_iso64(d, iso, text=True))
        if not os.path.exists(iso):
            return summary()

        # --- The disk of a laptop: GPT, and an EFI partition with
        # somebody else's boot loader already in it
        make_gpt_disk(disk)
        made = fat_format(disk, ESP_START, ESP_SECTORS, fat32=True,
                          label="ESPTEST")
        check("partycja EFI sformatowana przez mtools (nie przez nas)", made)
        check("i wyszla FAT32 - bo tak wypada z liczby klastrow",
              fat_kind(disk, ESP_START) == "FAT32")
        fat_mkdirs(disk, ESP_START, ["/EFI", "/EFI/BOOT", "/EFI/Microsoft"])
        fat_put(disk, ESP_START, b"\x4d\x5a" + b"cudzy loader" * 100,
                "/EFI/Microsoft/bootmgfw.efi")
        fat_put(disk, ESP_START, LONG_TEXT, "/czytaj-mnie-koniecznie.txt")
        fat_put(disk, ESP_START, BIG, "/duzy.bin")

        q = Qemu(KERNEL, INITRD, cdrom=iso, disk=disk, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=90)

            out = q.command("fat", "klaster", timeout=15)
            check("system sam znajduje partycje EFI w tablicy GPT",
                  "partycja 1, sektor 2048" in out)
            check("i rozpoznaje ja jako FAT32 z etykieta ESPTEST",
                  "FAT32" in out and "ESPTEST" in out)
            m = re.search(r"razem (\d+) KiB, wolnych (\d+) KiB", out)
            check("rozmiar i wolne miejsce policzone z tablicy"
                  + (f" ({m.group(1)} KiB, wolne {m.group(2)} KiB)"
                     if m else ""),
                  m is not None and 90000 < int(m.group(1)) < 110000
                  and int(m.group(2)) < int(m.group(1))
                  and int(m.group(2)) > int(m.group(1)) - 1000)

            out = q.command("fatls /", "pozycji", timeout=15)
            check("katalog glowny widziany tak, jak go zapisano",
                  "EFI" in out and "duzy.bin" in out)
            check("dluga nazwa odczytana w calosci, nie jako CZYTAJ~1.TXT",
                  "czytaj-mnie-koniecznie.txt" in out)
            check("rozmiar pliku z pozycji katalogu",
                  "200000 B" in out)

            out = q.command("fatls /EFI", "pozycji", timeout=15)
            check("katalog o dlugiej nazwie ('Microsoft') tez",
                  "Microsoft" in out and "BOOT" in out)

            out = q.command("fatcat /czytaj-mnie-koniecznie.txt",
                            "partycji EFI", timeout=15)
            check("tresc pliku co do znaku",
                  LONG_TEXT.decode().strip() in out)

            # A file of two hundred kilobytes is four hundred clusters:
            # read by following a chain, and a chain followed wrongly
            # gives back bytes that look perfectly plausible
            out = q.command("fatsum /duzy.bin", "suma", timeout=30)
            m = re.search(r"(\d+) B, suma ([0-9a-f]+)", out)
            check(f"caly plik przez lancuch klastrow "
                  f"({m.group(1) if m else '?'} B)",
                  m is not None and int(m.group(1)) == len(BIG))
            check("i bajt w bajt ten sam, ktory zapisal ktos inny",
                  m is not None and int(m.group(2), 16) == rot_sum(BIG))

            # --- Writing. Everything below is checked by mtools after
            # the machine is switched off, not by asking the system
            # whether it thinks it succeeded.
            out = q.command("fatmkdir /EFI/ProtonOS", "fatmkdir", timeout=15)
            check("katalog zalozony", "nie udalo" not in out)
            out = q.command("fatput /EFI/ProtonOS/witaj.txt czesc-swiecie",
                            "fatput", timeout=15)
            check("plik zapisany", "nie udalo" not in out)
            out = q.command("fatput /protonos64-testowy.bin ABCDEFGH",
                            "fatput", timeout=15)
            check("plik o dlugiej nazwie zapisany", "nie udalo" not in out)
            out = q.command("fatcopy libproton.so /EFI/ProtonOS/libproton.so",
                            "fatcopy", timeout=40)
            check("plik z initrd przepisany na partycje EFI",
                  "nie udalo" not in out and "-> /EFI/ProtonOS" in out)
            out = q.command("fatrm /duzy.bin", "fatrm", timeout=20)
            check("plik usuniety", "nie udalo" not in out)

            out = q.command("fatls /", "pozycji", timeout=15)
            check("po usunieciu nie ma go w katalogu",
                  "duzy.bin" not in out
                  and "protonos64-testowy.bin" in out)
            q.command("fat", "klaster", timeout=15)
        finally:
            q.close()

        # --- What is really on the disk, according to somebody else
        listing = fat_dir(disk, ESP_START)
        check("mtools czyta katalog zalozony przez nas",
              "ProtonOS" in listing)
        check("mtools czyta dluga nazwe zapisana przez nas",
              "protonos64-testowy.bin" in listing)
        check("dluga nazwa ma tez poprawna nazwe krotka (~1)",
              re.search(r"PROTON~\d\s+BIN", listing) is not None)
        check("plik, ktory usunelismy, zniknal tez dla mtools",
              "duzy" not in listing)
        got = fat_get(disk, ESP_START, "/EFI/ProtonOS/witaj.txt")
        check("tresc zapisanego pliku, odczytana przez mtools",
              got is not None and got.strip() == b"czesc-swiecie")
        got = fat_get(disk, ESP_START, "/protonos64-testowy.bin")
        check("i pliku o dlugiej nazwie",
              got is not None and got.strip() == b"ABCDEFGH")

        # The copy from the initrd, byte for byte against the file the
        # test itself can read out of the initrd image
        want = None
        with open(INITRD64, "rb") as fh:
            blob = fh.read()
        import struct as _s
        count = _s.unpack_from("<I", blob, 4)[0]
        for i in range(count):
            off = 8 + i * 24
            name = blob[off:off + 16].split(b"\0")[0].decode()
            o, sz = _s.unpack_from("<II", blob, off + 16)
            if name == "libproton.so":
                want = blob[o:o + sz]
        got = fat_get(disk, ESP_START, "/EFI/ProtonOS/libproton.so")
        check(f"plik z initrd przepisany co do bajtu "
              f"({len(want) if want else 0} B)",
              want is not None and got == want)

        check("cudzy loader na partycji EFI nietkniety",
              (fat_get(disk, ESP_START, "/EFI/Microsoft/bootmgfw.efi") or b"")
              .startswith(b"\x4d\x5a"))
        check("tablica GPT nietknieta", gpt_intact(disk))
        # The EFI partition is the one the test itself put a filesystem
        # into, so the neighbour that still has to be untouched is the
        # Windows one behind it
        check("sasiednia partycja Windows nietknieta",
              neighbours_intact(disk, which=(1,)))

        # --- The old table and the two smaller formats. FAT12 is not a
        # curiosity here: its entries are twelve bits, so one of them
        # straddles every sector boundary of the table, and that is
        # where a driver written for whole entries goes wrong.
        for name, path, mib, want_kind in (("FAT16", small, 64, "FAT16"),
                                           ("FAT12", tiny, 2, "FAT12")):
            ok, start = make_mbr_fat_disk(path, mib, fat32=False,
                                          label="MALYFAT")
            check(f"obraz {name} w tablicy MBR gotowy",
                  ok and fat_kind(path, start) == want_kind)
            fat_put(path, start, LONG_TEXT, "/od-mtools.txt")

            q2 = Qemu(KERNEL, INITRD, cdrom=iso, disk=path, serial=slog2,
                      qemu="qemu-system-x86_64")
            try:
                q2.wait_serial("proton64>", timeout=90)
                out = q2.command("fat 1", "klaster", timeout=20)
                check(f"{name} rozpoznany przez liczbe klastrow, "
                      f"nie przez napis w sektorze rozruchowym",
                      want_kind in out)
                out = q2.command("fatcat /od-mtools.txt", "partycji",
                                 timeout=15)
                check(f"{name}: plik od mtools odczytany",
                      LONG_TEXT.decode().strip() in out)
                out = q2.command("fatcopy hello64 /hello64-z-initrd.bin",
                                 "fatcopy", timeout=40)
                check(f"{name}: zapis pliku przez lancuch klastrow",
                      "nie udalo" not in out)
                out = q2.command("fatls /", "pozycji", timeout=15)
                check(f"{name}: oba pliki w katalogu glownym",
                      "od-mtools.txt" in out
                      and "hello64-z-initrd.bin" in out)
            finally:
                q2.close()

            listing = fat_dir(path, start)
            check(f"{name}: mtools widzi to, co zapisalismy",
                  "hello64-z-initrd.bin" in listing)
            got = fat_get(path, start, "/hello64-z-initrd.bin")
            check(f"{name}: i tresc zgadza sie z plikiem w initrd",
                  got is not None and len(got) > 4000
                  and got[:4] == b"\x7fELF")

        # --- And the thing this driver must never do
        check("na zadnym z obrazow nie zniknal sektor rozruchowy FAT",
              fat_kind(disk, ESP_START) == "FAT32"
              and fat_kind(small, 2048) == "FAT16"
              and fat_kind(tiny, 2048) == "FAT12")
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, disk, small, tiny, slog, slog2):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
