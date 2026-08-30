#!/usr/bin/env python3
"""Test dymny ProtonOS: biblioteka wspoldzielona i wiazanie symboli.

Do tej pory kazdy program niosl w sobie wszystko, czego uzywal.
Kryptografia i TLS znaczyly po kilkadziesiat kilobajtow tego samego
kodu w kazdym z kilku programow - raz w initrd i raz w pamieci,
osobno dla kazdego procesu.

Teraz kod lezy raz, pod stalym adresem, w ramkach wspolnych dla
wszystkich przestrzeni adresowych. Program ma zamiast niego tablice
pustych miejsc, ktore jadro wypelnia przy ladowaniu.

Sprawdzamy to rzeczami, ktorych nie da sie sfalszowac:

  1. Kod, ktory program wykonuje, lezy POZA jego obrazem. Adres musi
     podac sama biblioteka: program nieprzemieszczalny, ktory bierze
     adres jej funkcji, dostaje adres wlasnej zaslepki skoku.
  2. Wynik jest poprawny - SHA-256('abc') z FIPS 180-4. Skok przez
     niewypelniona pozycje nie ma jak dac wlasciwej odpowiedzi.
  3. Dane biblioteki sa PRYWATNE. Licznik generatora losowego zaczyna
     w kazdym procesie od zera; gdyby dane byly wspolne, drugi bieg
     zastalby licznik pierwszego.
  4. Kodu biblioteki nie da sie nadpisac. Ramki sa wspolne, wiec zapis
     do nich podmienialby funkcje wszystkim naraz - ma sie nie udac
     i ma zabic tego, kto probuje.
  5. Programy naprawde schudly: cztery najciezsze razem waza mniej
     niz sama biblioteka.

Oba jadra dostaja swoja biblioteke z tych samych naglowkow - rozny
jest tylko adres i szerokosc rejestrow.

Uruchamiany osobno albo przez scripts/run_testy.py, ktory puszcza
wszystkie grupy rownolegle.
"""

import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def size(listing, name):
    """Rozmiar pliku z wypisu 'ls' initrd."""
    m = re.search(r"\b" + re.escape(name) + r"\s+\((\d+) B\)", listing)
    return int(m.group(1)) if m else None


def adresy(text):
    """Trzy adresy, ktore wypisuje dynlib."""
    def one_core(co):
        m = re.search(r"kod programu pod (\d+)" if co == "prog" else
                      (r"kod biblioteki pod (\d+)" if co == "kod" else
                       r"dane biblioteki pod (\d+)"), text)
        return int(m.group(1)) if m else None
    return one_core("prog"), one_core("kod"), one_core("dane")


def check_shared(q, marker, suffix, lib_base):
    """Wspolna czesc dla obu jader: program dynlib i wypis initrd."""
    first = q.command("run dynlib", marker, timeout=60)
    prog, code, dane = adresy(first)
    check("program podaje wszystkie trzy adresy" + suffix,
          prog is not None and code is not None and dane is not None)
    if code is None:
        return

    # Kod biblioteki lezy w JEJ obszarze, a nie w obrazie programu.
    # To jest cala rzecz: program tego kodu w sobie nie ma.
    check("kod biblioteki lezy poza obrazem programu" + suffix,
          code >= lib_base and code < lib_base + 16 * 1024 * 1024)
    check("kod programu lezy tam, gdzie zawsze" + suffix,
          prog is not None and 0x40000000 <= prog < 0x41000000)
    check("dane biblioteki sa w osobnym, wyzszym segmencie" + suffix,
          dane is not None and dane > code)
    check("kryptografia z biblioteki liczy poprawnie" + suffix,
          "zgadza sie z FIPS 180-4" in first and "ZLE" not in first)

    counter = re.search(r"licznik po pobraniu bajtow: (\d+)", first)
    check("program doszedl do konca" + suffix,
          "[dynlib] gotowe" in first and counter is not None)

    # Drugi bieg: ten sam kod pod tym samym adresem (ramki wspolne),
    # ale licznik znow od zera (dane prywatne). Gdyby dane byly wspolne,
    # zastalby stan poprzedniego procesu.
    second = q.command("run dynlib", marker, timeout=60)
    prog2, code2, dane2 = adresy(second)
    counter2 = re.search(r"licznik po pobraniu bajtow: (\d+)", second)
    check("drugi proces widzi kod pod tym samym adresem" + suffix,
          code2 == code and dane2 == dane)
    check("ale dane biblioteki dostaje wlasne, od zera" + suffix,
          counter is not None and counter2 is not None and
          counter2.group(1) == counter.group(1))

    # Zapis do wspolnego kodu musi zabic tego, kto probuje
    corrupt = q.command("run dynlib pisz", "Page Fault", timeout=60)
    check("zapis do kodu biblioteki konczy sie bledem ochrony" + suffix,
          "probuje pisac po kodzie" in corrupt and "Page Fault" in corrupt and
          "zapis do kodu biblioteki sie udal" not in corrupt)

    # Rozmiary: cztery najciezsze programy razem lzejsze niz biblioteka
    listing = q.command("ls", "dynlib", timeout=20)
    lib = size(listing, "libproton.so")
    check("biblioteka jest w initrd i wazy swoje" + suffix,
          lib is not None and lib > 60000)
    heavy = [size(listing, n)
               for n in ("cryptotest", "certtest", "httpget", "gweb")]
    check("wszystkie cztery programy sa w wypisie" + suffix,
          all(v is not None for v in heavy))
    if lib and all(v is not None for v in heavy):
        check("cztery najciezsze programy razem lzejsze niz biblioteka"
              " (%d B < %d B)%s" % (sum(heavy), lib, suffix),
              sum(heavy) < lib)


def main():
    print("--- biblioteka wspoldzielona (jadro 64-bitowe) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
    else:
        d = tempfile.mkdtemp()
        iso = tempfile.mktemp(suffix=".iso")
        slog = tempfile.mktemp(suffix=".log")
        try:
            check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
            if not os.path.exists(iso):
                return summary()

            q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                     qemu="qemu-system-x86_64")
            try:
                start = q.wait_serial("proton64>", timeout=90)
                m = re.search(r"Biblioteka libproton\.so: (\d+) stron", start)
                check("jadro wczytalo biblioteke przy rozruchu",
                      m is not None and int(m.group(1)) >= 8)
                check_shared(q, "kod wyjscia", "", 0x4F000000)
            finally:
                q.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)
            for f in (iso, slog):
                if os.path.exists(f):
                    os.unlink(f)

    # --- To samo w jadrze 32-bitowym ---
    print("--- biblioteka wspoldzielona (jadro 32-bitowe) ---")
    slog32 = tempfile.mktemp(suffix=".log")
    disk32 = tempfile.mktemp(suffix=".img")
    with open(disk32, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        q3 = Qemu(KERNEL, INITRD, disk32, append=TEXT_MODE, serial=slog32)
        try:
            start = q3.wait_for("ProtonOS shell", timeout=40)
            m = re.search(r"Biblioteka libproton\.so: (\d+) stron", start)
            check("jadro wczytalo biblioteke przy rozruchu (32-bit)",
                  m is not None and int(m.group(1)) >= 8)
            q3.wait_for("[siec] DHCP", timeout=30)
            check_shared(q3, "zakonczyl sie", " (32-bit)", 0xAF000000)
        finally:
            q3.close()
    finally:
        for f in (slog32, disk32):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
