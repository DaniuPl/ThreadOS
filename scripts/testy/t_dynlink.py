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
from ptest import check, wynik, zrob_iso64, brak64


def rozmiar(listing, nazwa):
    """Rozmiar pliku z wypisu 'ls' initrd."""
    m = re.search(r"\b" + re.escape(nazwa) + r"\s+\((\d+) B\)", listing)
    return int(m.group(1)) if m else None


def adresy(tekst):
    """Trzy adresy, ktore wypisuje dynlib."""
    def jeden(co):
        m = re.search(r"kod programu pod (\d+)" if co == "prog" else
                      (r"kod biblioteki pod (\d+)" if co == "kod" else
                       r"dane biblioteki pod (\d+)"), tekst)
        return int(m.group(1)) if m else None
    return jeden("prog"), jeden("kod"), jeden("dane")


def sprawdz_wspolne(q, marker, dopisek, baza_lib):
    """Wspolna czesc dla obu jader: program dynlib i wypis initrd."""
    pierwszy = q.polecenie("run dynlib", marker, timeout=60)
    prog, kod, dane = adresy(pierwszy)
    check("program podaje wszystkie trzy adresy" + dopisek,
          prog is not None and kod is not None and dane is not None)
    if kod is None:
        return

    # Kod biblioteki lezy w JEJ obszarze, a nie w obrazie programu.
    # To jest cala rzecz: program tego kodu w sobie nie ma.
    check("kod biblioteki lezy poza obrazem programu" + dopisek,
          kod >= baza_lib and kod < baza_lib + 16 * 1024 * 1024)
    check("kod programu lezy tam, gdzie zawsze" + dopisek,
          prog is not None and 0x40000000 <= prog < 0x41000000)
    check("dane biblioteki sa w osobnym, wyzszym segmencie" + dopisek,
          dane is not None and dane > kod)
    check("kryptografia z biblioteki liczy poprawnie" + dopisek,
          "zgadza sie z FIPS 180-4" in pierwszy and "ZLE" not in pierwszy)

    licznik = re.search(r"licznik po pobraniu bajtow: (\d+)", pierwszy)
    check("program doszedl do konca" + dopisek,
          "[dynlib] gotowe" in pierwszy and licznik is not None)

    # Drugi bieg: ten sam kod pod tym samym adresem (ramki wspolne),
    # ale licznik znow od zera (dane prywatne). Gdyby dane byly wspolne,
    # zastalby stan poprzedniego procesu.
    drugi = q.polecenie("run dynlib", marker, timeout=60)
    prog2, kod2, dane2 = adresy(drugi)
    licznik2 = re.search(r"licznik po pobraniu bajtow: (\d+)", drugi)
    check("drugi proces widzi kod pod tym samym adresem" + dopisek,
          kod2 == kod and dane2 == dane)
    check("ale dane biblioteki dostaje wlasne, od zera" + dopisek,
          licznik is not None and licznik2 is not None and
          licznik2.group(1) == licznik.group(1))

    # Zapis do wspolnego kodu musi zabic tego, kto probuje
    psuj = q.polecenie("run dynlib pisz", "Page Fault", timeout=60)
    check("zapis do kodu biblioteki konczy sie bledem ochrony" + dopisek,
          "probuje pisac po kodzie" in psuj and "Page Fault" in psuj and
          "zapis do kodu biblioteki sie udal" not in psuj)

    # Rozmiary: cztery najciezsze programy razem lzejsze niz biblioteka
    listing = q.polecenie("ls", "dynlib", timeout=20)
    lib = rozmiar(listing, "libproton.so")
    check("biblioteka jest w initrd i wazy swoje" + dopisek,
          lib is not None and lib > 60000)
    ciezkie = [rozmiar(listing, n)
               for n in ("cryptotest", "certtest", "httpget", "gweb")]
    check("wszystkie cztery programy sa w wypisie" + dopisek,
          all(v is not None for v in ciezkie))
    if lib and all(v is not None for v in ciezkie):
        check("cztery najciezsze programy razem lzejsze niz biblioteka"
              " (%d B < %d B)%s" % (sum(ciezkie), lib, dopisek),
              sum(ciezkie) < lib)


def main():
    print("--- biblioteka wspoldzielona (jadro 64-bitowe) ---")

    powod = brak64()
    if powod:
        print("POMINIETE: " + powod)
    else:
        d = tempfile.mkdtemp()
        iso = tempfile.mktemp(suffix=".iso")
        slog = tempfile.mktemp(suffix=".log")
        try:
            check("plyta z jadrem 64-bitowym buduje sie", zrob_iso64(d, iso))
            if not os.path.exists(iso):
                return wynik()

            q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                     qemu="qemu-system-x86_64")
            try:
                start = q.wait_serial("proton64>", timeout=90)
                m = re.search(r"Biblioteka libproton\.so: (\d+) stron", start)
                check("jadro wczytalo biblioteke przy rozruchu",
                      m is not None and int(m.group(1)) >= 8)
                sprawdz_wspolne(q, "kod wyjscia", "", 0x4F000000)
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
            sprawdz_wspolne(q3, "zakonczyl sie", " (32-bit)", 0xAF000000)
        finally:
            q3.close()
    finally:
        for f in (slog32, disk32):
            if os.path.exists(f):
                os.unlink(f)

    return wynik()


if __name__ == "__main__":
    sys.exit(main())
