#!/usr/bin/env python3
"""Test dymny ProtonOS: programy uzytkownika na wszystkich rdzeniach.

Watki jadra chodzily po wszystkich rdzeniach juz wczesniej. Program
w pierscieniu 3 to jednak co innego: potrzebuje wlasnego segmentu stanu
zadania na kazdym rdzeniu, wlasnego wejscia w wywolania systemowe -
i, co okazalo sie najwazniejsze, WLASCZONEGO STRONICOWANIA. Rdzen
wtorny w trybie chronionym startowal bez niego: dla kodu jadra wyszlo to
na jedno, bo dolna pamiec jest odwzorowana jeden do jednego, ale adres
programu uzytkownika bez tablic stron nie znaczyl nic i proces ginal.

Sprawdzamy cztery rzeczy:

  1. 'run cores' - dwa procesy trzymane na rdzeniu rozruchowym. Maska
     rdzeni ma byc rowna 1, czyli mechanizm domyslnie NIE rozprasza.
  2. 'runw cores' - te same dwa procesy, wolno im biec wszedzie. Maska
     ma miec wiecej niz jeden bit, czyli pierscien 3 naprawde byl poza
     rdzeniem rozruchowym.
  3. Czas. Oba procesy licza tyle samo, wiec na jednym rdzeniu kazdy
     widzi dwa razy dluzej niz na dwoch. Porownujemy 'run' z 'runw'
     w tej samej maszynie, wiec nie mierzymy szybkosci gospodarza.
  4. Suma kontrolna. Program, ktory policzyl swoje na innym rdzeniu, ma
     dac dokladnie te sama liczbe, co policzony na rdzeniu rozruchowym.
     Bez tego punkty 2 i 3 mowilyby tylko, ze cos szlo szybciej.

Na maszynie jednordzeniowej 'runw' ma dzialac tak samo jak 'run' -
obietnica "wolno wszedzie" nie jest obietnica "musi gdzie indziej".

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

WZOR = re.compile(r"\[cores\] (dziecko|rodzic ): rdzenie (\d+), "
                  r"tyknac (\d+), suma (\d+)")


def rozbierz(out):
    """[(maska, tyknac, suma), ...] dla obu procesow."""
    return [(int(m.group(2)), int(m.group(3)), int(m.group(4)))
            for m in WZOR.finditer(out)]


def sprawdz_pare(out, etykieta):
    """Wspolne dla 'run' i 'runw': oba procesy doszly do konca i zgadzaja
    sie co do wyniku. Zwraca (maska_lacznie, najwiecej_tykniec, suma)."""
    proc = rozbierz(out)
    check("oba procesy doszly do konca" + etykieta, len(proc) == 2)
    if len(proc) != 2:
        return None

    sumy = set(p[2] for p in proc)
    check("oba policzyly dokladnie to samo" + etykieta, len(sumy) == 1)

    maska = proc[0][0] | proc[1][0]
    tyknac = max(p[1] for p in proc)
    return maska, tyknac, sumy.pop()


def proba(q, rdzeni, etykieta):
    """Jedna maszyna: 'run' obok 'runw' i porownanie."""
    naraz = None

    po_kolei = sprawdz_pare(
        q.polecenie("run cores", "gotowe", timeout=180), " ('run')" + etykieta)
    if po_kolei:
        maska, tyknac, _ = po_kolei
        check("bez 'runw' oba procesy zostaly na rdzeniu rozruchowym "
              "(maska %d)%s" % (maska, etykieta), maska == 1)
        check("porcja odniesienia trwala dosc dlugo, by cos znaczyc "
              "(%d tyknac)%s" % (tyknac, etykieta), tyknac >= 20)

    naraz = sprawdz_pare(
        q.polecenie("runw cores", "gotowe", timeout=180),
        " ('runw')" + etykieta)
    if not (po_kolei and naraz):
        return

    check("ta sama suma kontrolna z 'run' i z 'runw'" + etykieta,
          po_kolei[2] == naraz[2])

    maska = naraz[0]
    if rdzeni > 1:
        check("program w pierscieniu 3 biegl takze poza rdzeniem "
              "rozruchowym (maska %d)%s" % (maska, etykieta),
              (maska & ~1) != 0)
        check("prowadzil go wiecej niz jeden rdzen (%d)%s"
              % (bin(maska).count("1"), etykieta),
              bin(maska).count("1") >= 2)
        # Dwa procesy na jednym rdzeniu licza sie po kolei, na dwoch -
        # naraz. Prog stoi w polowie drogi miedzy jednym a drugim.
        check("dwa procesy naraz szybciej niz po kolei (%d wobec %d "
              "tykniec)%s" % (naraz[1], po_kolei[1], etykieta),
              naraz[1] < 3 * po_kolei[1] // 4)
    else:
        check("na jednej maszynie jednordzeniowej 'runw' nie zmienia nic "
              "(maska %d)%s" % (maska, etykieta), maska == 1)
        check("i trwa tyle samo, co 'run' (%d wobec %d tykniec)%s"
              % (naraz[1], po_kolei[1], etykieta),
              naraz[1] > po_kolei[1] // 2)


def main():
    print("--- procesy na wszystkich rdzeniach (jadro 64-bitowe) ---")

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

            q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, cpus=4,
                     qemu="qemu-system-x86_64")
            try:
                q.wait_serial("proton64>", timeout=90)
                proba(q, 4, "")
                dalej = q.polecenie("run hello64", "kod wyjscia", timeout=60)
                check("jadro dziala dalej po probie",
                      "czesc, jestem ELF-em" in dalej)
            finally:
                q.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)
            for f in (iso, slog):
                if os.path.exists(f):
                    os.unlink(f)

    print("--- procesy na wszystkich rdzeniach (jadro 32-bitowe) ---")
    slog32 = tempfile.mktemp(suffix=".log")
    try:
        q3 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog32, cpus=4)
        try:
            q3.wait_for("ProtonOS shell", timeout=40)
            proba(q3, 4, " (32-bit)")
            dalej = q3.polecenie("run hello", "zakonczyl sie", timeout=60)
            check("jadro dziala dalej po probie (32-bit)",
                  "zakonczyl sie (kod 0)" in dalej)
        finally:
            q3.close()
    finally:
        if os.path.exists(slog32):
            os.unlink(slog32)

    print("--- ta sama proba na jednym rdzeniu ---")
    slog1 = tempfile.mktemp(suffix=".log")
    try:
        q1 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog1)
        try:
            q1.wait_for("ProtonOS shell", timeout=40)
            proba(q1, 1, " (jeden rdzen)")
        finally:
            q1.close()
    finally:
        if os.path.exists(slog1):
            os.unlink(slog1)

    return wynik()


if __name__ == "__main__":
    sys.exit(main())
