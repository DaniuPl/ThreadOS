#!/usr/bin/env python3
"""ProtonOS smoke test: a disk that is not ours is not ours to take.

Until this group existed, fs_init() did something that was harmless for
as long as every disk this system ever saw was an empty file made by a
test, and catastrophic the first time it met a real one: if the first
block did not say PFS3, it FORMATTED the disk. No question, no look at
what was there.

A laptop's disk does not say PFS3 either. Booting this system from a
stick would have thrown away the partition table, the Windows and
everything else, in the first second, without asking.

So the assertions are about restraint, and about being able to say what
one is being restrained from:

  a disk with a partition table on it comes out of a boot with that
  table still on it - read back from the host, byte for byte, not
  taken on the system's word
  and the machine says why it left it alone
  it can READ what is there: three partitions, their kinds, their
  sizes and the names they carry
  a genuinely blank disk is still formatted, because that is a disk
  nobody loses anything by
  and 'formatuj' does it on purpose - but only for somebody who types
  the whole word
  'formatuj N' takes ONE partition: the table and the neighbours come
  out of it untouched, and after a restart the system finds its
  filesystem where it left it - which is what installing next to
  somebody else's Windows actually means

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, make_gpt_disk, gpt_intact,
                   neighbours_intact)


def main():
    disk = tempfile.mktemp(suffix=".img")
    slog = tempfile.mktemp(suffix=".log")
    make_gpt_disk(disk, 800)

    try:
        check("obraz testowy naprawde ma tablice GPT", gpt_intact(disk))

        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
        try:
            boot = q.wait_for("ProtonOS shell", timeout=40)
            check("jadro mowi, ze na dysku cos jest i tego nie rusza",
                  "nie ruszam go" in boot)
            check("i podpowiada, co zrobic, gdyby jednak mial byc nasz",
                  "formatuj" in boot)

            # What it can read. Not the point of the group, but the
            # thing that makes the restraint useful rather than blind.
            q.type_line("sprzet")
            time.sleep(3)
            said = q.screen()
            check("czyta tablice GPT i liczbe partycji ('%s')"
                  % next((l.strip() for l in said.splitlines()
                          if "tablica" in l), "brak wiersza"),
                  "tablica GPT" in said and "partycji: 3" in said)
            check("nazywa partycje EFI i widzi jej etykiete",
                  "EFI (ESP)" in said and "EFI system partition" in said)
            check("i rozpoznaje partycje danych oraz Linuksa",
                  "Windows" in said and "linux-root" in said)
        finally:
            q.close()

        # The assertion the whole group exists for: read the image back
        # from the host, and the table has to still be in it.
        check("po calym rozruchu tablica partycji jest NIETKNIETA",
              gpt_intact(disk))
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    # --- and a disk nobody loses anything by --------------------------
    blank = tempfile.mktemp(suffix=".img")
    slog2 = tempfile.mktemp(suffix=".log")
    with open(blank, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    try:
        q = Qemu(KERNEL, INITRD, blank, append=TEXT_MODE, serial=slog2)
        try:
            boot = q.wait_for("ProtonOS shell", timeout=40)
            check("pusty dysk jest formatowany tak, jak byl dotad",
                  "sformatowany" in boot)
            check("i nie ma tam mowy o odmowie", "nie ruszam go" not in boot)
        finally:
            q.close()
    finally:
        for f in (blank, slog2):
            if os.path.exists(f):
                os.unlink(f)

    # --- living inside a partition, next to somebody else -------------
    # This is what installing on a laptop actually means: one partition
    # becomes ours, the table and the neighbours stay exactly as they
    # were, and after a restart the system finds its filesystem where
    # it left it.
    disk3 = tempfile.mktemp(suffix=".img")
    slog4 = tempfile.mktemp(suffix=".log")
    make_gpt_disk(disk3, 800)
    try:
        q = Qemu(KERNEL, INITRD, disk3, append=TEXT_MODE, serial=slog4)
        try:
            q.wait_for("ProtonOS shell", timeout=40)
            q.type_line("formatuj 3")
            time.sleep(2)
            said = q.screen()
            check("'formatuj 3' mowi, ze skasuje TE partycje, nie dysk",
                  "SKASUJE PARTYCJE 3" in said and
                  "tablica partycji zostaja nietkniete" in said)
            q.type_line("TAK")
            time.sleep(4)
            said = q.screen()
            check("i zaklada na niej PFS",
                  "ta partycja jest teraz PFS-em" in said)

            q.type_line("write dowod.txt zainstalowany-obok-windowsa")
            time.sleep(2)
        finally:
            q.close()

        check("tablica partycji przezyla formatowanie partycji",
              gpt_intact(disk3))
        check("i sasiednie partycje tez - dalej maja swoje dane",
              neighbours_intact(disk3))

        # And the system finds it again on the next boot
        q = Qemu(KERNEL, INITRD, disk3, append=TEXT_MODE, serial=slog4)
        try:
            boot = q.wait_for("ProtonOS shell", timeout=40)
            check("po restarcie jadro znajduje PFS w partycji ('%s')"
                  % next((l.strip() for l in boot.splitlines()
                          if "w partycji" in l), "brak wiersza"),
                  "PFS: w partycji 3" in boot)
            out = q.command("cat dowod.txt", "zainstalowany", timeout=20)
            check("a plik zapisany przed restartem tam jest",
                  "zainstalowany-obok-windowsa" in out)
        finally:
            q.close()
    finally:
        for f in (disk3, slog4):
            if os.path.exists(f):
                os.unlink(f)

    # --- formatting on purpose ---------------------------------------
    # The disk with the table again: this time somebody asks for it, and
    # a wrong answer has to leave the table where it is.
    disk2 = tempfile.mktemp(suffix=".img")
    slog3 = tempfile.mktemp(suffix=".log")
    make_gpt_disk(disk2, 800)
    try:
        q = Qemu(KERNEL, INITRD, disk2, append=TEXT_MODE, serial=slog3)
        try:
            q.wait_for("ProtonOS shell", timeout=40)

            q.type_line("formatuj")
            time.sleep(2)
            q.type_line("tak")            # not the word that is asked for
            time.sleep(3)
            said = q.screen()
            check("'formatuj' ostrzega, co skasuje ('%s')"
                  % next((l.strip() for l in said.splitlines()
                          if "SKASUJE" in l), "brak wiersza"),
                  "SKASUJE CALY DYSK" in said)
            check("zla odpowiedz niczego nie kasuje",
                  "nie skasowano niczego" in said)
        finally:
            q.close()
        check("i tablica dalej jest na miejscu", gpt_intact(disk2))

        # And now the word itself
        q = Qemu(KERNEL, INITRD, disk2, append=TEXT_MODE, serial=slog3)
        try:
            q.wait_for("ProtonOS shell", timeout=40)
            q.type_line("formatuj")
            time.sleep(2)
            q.type_line("TAK")
            time.sleep(4)
            said = q.screen()
            check("a wpisane TAK formatuje dysk",
                  "caly dysk jest teraz PFS-em" in said)
        finally:
            q.close()
        check("i wtedy tablicy GPT juz nie ma - o to sie prosilo",
              not gpt_intact(disk2))
    finally:
        for f in (disk2, slog3):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
