#!/usr/bin/env python3
"""Test dymny ProtonOS: pliki odwzorowane w pamieci.

Loader ELF i system plikow polaczylismy juz wczesniej - program moze
lezec na dysku. To jest druga polowa tej samej mysli: plik moze byc
PAMIECIA. Program dostaje adres i czyta zawartosc jak zwykla tablice,
bez ani jednego wywolania read.

Sprawdzamy trzy rzeczy, kazda innym sposobem:

  1. bajty pod adresem zgadzaja sie z tym, co lezy w pliku,
  2. strony przychodza POD PALCAMI - dotkniecie jednego bajtu
     stustronicowego pliku ma kosztowac jedna strone, nie sto,
  3. odwzorowanie jest tylko do odczytu i proba zapisu ma zabic
     proces, a nie po cichu przepasc.

Numery blokow jadro ustala raz, przy zakladaniu odwzorowania, i czyta
je potem warstwa blokowa z pominieciem kodu PFS - z tego samego
powodu, co przy wymianie stron: PFS ma jeden wspolny bufor na blok,
a blad strony potrafi trafic w srodku dowolnej operacji na plikach.

Oba jadra maja to samo, kazde swoim kodem stronicowania - i oba
sprawdzamy tak samo. Przy okazji sprawdzamy tu drugi obszar rosnacy
pod palcami: stos. Program dostaje osiem stron, a schodzi rekurencja
znacznie glebiej.
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
    """Ile stron jadro wczytalo z odwzorowanych plikow - shell podaje
    to w podsumowaniu po kazdym programie."""
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

            # --- Maly plik: caly czytany przez odwzorowanie ---
            make_it = q.command("run mkmapa64 mapa.txt 12", "kod wyjscia",
                               timeout=60)
            check("plik z wzorcem zalozony na dysku",
                  "[mkmapa64] zapisano 12288 B wzorca" in make_it)

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

            # --- Duzy plik, jeden dotkniety bajt ---
            # To jest wlasciwy dowod na stronicowanie na zadanie:
            # odwzorowanie kosztuje tyle, ile sie z niego wzielo
            q.command("run mkmapa64 duza.txt 400", "kod wyjscia",
                        timeout=120)
            malo = q.command("run mmapfile64 duza.txt 1", "kod wyjscia",
                               timeout=90)
            check("odwzorowanie duzego pliku sie udaje",
                  "reszty pliku nie tknalem" in malo)
            check("dotkniecie jednego bajtu kosztuje jedna strone, "
                  "a nie caly plik", pages_from_files(malo) == 1)

            # ...a przeczytanie calego kosztuje juz tyle, ile ma stron
            many = q.command("run mmapfile64 duza.txt", "kod wyjscia",
                               timeout=120)
            check("przeczytanie calego pliku wciaga wszystkie jego strony",
                  pages_from_files(many) == 400 * 1024 // 4096)

            # Odwzorowanie nie przezywa procesu: pamiec wraca do puli
            m = re.search(r"mmapfile64\] koniec, kod wyjscia 0 "
                          r"\(ramek (\d+) -> (\d+)", many)
            check("po koncu programu ramki wrocily do puli",
                  m is not None and m.group(1) == m.group(2))

            # Plik, ktorego nie ma, jest bledem, a nie pustym adresem
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

    # --- To samo w jadrze 32-bitowym ---
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

            make_it = q3.command("run mkmapa mapa.txt 12", "zakonczyl sie",
                                timeout=60)
            check("plik z wzorcem zalozony na dysku (32-bit)",
                  "[mkmapa] zapisano 12288 B wzorca" in make_it)

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

            # Stronicowanie na zadanie: duzy plik, jeden dotkniety bajt
            before = q3.command("mem", "z plikow:")
            m1 = re.search(r"z plikow:\s+(\d+)", before)
            q3.command("run mkmapa duza.txt 400", "zakonczyl sie",
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

            # Stos tez rosnie pod palcami. Program dostaje osiem stron,
            # a schodzi rekurencja grubo glebiej - i po powrocie
            # sprawdza, czy kazda ramka jest wciaz jego. Dokladanie
            # stron pod nadpisywanie cudzych ramek wygladaloby
            # w polowie tak samo.
            stack = q3.command("run gleboko", "zakonczyl sie", timeout=60)
            check("stos rosnie na zadanie: rekurencja siega poza osiem "
                  "stron", "[gleboko] zeszlem na poziom 40" in stack)
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
