#!/usr/bin/env python3
"""Test dymny ProtonOS: wywolania systemowe bez wielkiego zamka jadra.

Poprzedni krok wpuscil programy uzytkownika na wszystkie rdzenie, ale
kazde wejscie do jadra bralo jeden zamek na cale jadro. Liczenie
w pierscieniu 3 szlo wiec rownolegle, a wszystko, co dotykalo jadra -
po kolei. Program, ktory czesto wola jadro, nie zyskiwal na czterech
rdzeniach nic; tracil, bo do czekania dochodzilo bicie sie o te sama
linie pamieci.

Teraz wywolanie, ktore siega wylacznie po stan wolajacego zadania,
przechodzi bez zamka. Lista takich wywolan jest krotka i jawna, a
domyslna odpowiedz brzmi "z zamkiem".

Sprawdzamy trzy rzeczy:

  1. Poprawnosc, i to jest najwazniejsze. Kazde z miliona wywolan
     sys_getpid ma zwrocic numer TEGO procesu. Gdyby dwa rdzenie
     mylily sie co do tego, ktore zadanie prowadza - a to jest dokladnie
     to ryzyko, ktore zamek do tej pory zaslanial - widac by to bylo
     tutaj natychmiast.
  2. 'run syscalls' trzyma oba procesy na rdzeniu rozruchowym, wiec
     maska rdzeni ma byc rowna 1. To jest pomiar odniesienia.
  3. 'runw syscalls' pozwala im biec wszedzie. Ma byc WYRAZNIE
     szybciej, a nie tylko troche - i to jest cala roznica, bo przed
     ta zmiana bylo w tym miejscu WOLNIEJ niz na jednym rdzeniu.

Na maszynie jednordzeniowej obie komendy maja dac to samo.

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

WZOR = re.compile(r"\[syscalls\] (dziecko|rodzic ): rdzenie (\d+), "
                  r"tyknac (\d+), wywolan (\d+), zgodnych (\d+)")


def zmierz(q, komenda, etykieta):
    """Jedno uruchomienie: (maska_lacznie, najdluzszy_czas) albo None."""
    out = q.polecenie(komenda, "gotowe", timeout=300)
    proc = [(int(m.group(2)), int(m.group(3)), int(m.group(4)),
             int(m.group(5))) for m in WZOR.finditer(out)]

    check("oba procesy doszly do konca" + etykieta, len(proc) == 2)
    if len(proc) != 2:
        return None

    zle = [(w, z) for _, _, w, z in proc if w != z]
    check("kazde wywolanie zwrocilo numer wlasnego procesu "
          "(%d + %d razy)%s" % (proc[0][3], proc[1][3], etykieta),
          not zle and proc[0][3] > 0)

    return (proc[0][0] | proc[1][0], max(p[1] for p in proc))


def proba(q, rdzeni, etykieta):
    po_kolei = zmierz(q, "run syscalls", " ('run')" + etykieta)
    if po_kolei:
        check("bez 'runw' oba procesy zostaly na rdzeniu rozruchowym "
              "(maska %d)%s" % (po_kolei[0], etykieta), po_kolei[0] == 1)
        check("pomiar odniesienia trwal dosc dlugo, by cos znaczyc "
              "(%d tyknac)%s" % (po_kolei[1], etykieta), po_kolei[1] >= 20)

    naraz = zmierz(q, "runw syscalls", " ('runw')" + etykieta)
    if not (po_kolei and naraz):
        return

    if rdzeni > 1:
        check("wywolania szly takze poza rdzeniem rozruchowym (maska %d)%s"
              % (naraz[0], etykieta), (naraz[0] & ~1) != 0)
        # Prog jest z zapasem, ale nie z takim, zeby przepuscil brak
        # zmiany: pod wielkim zamkiem wychodzilo tu WIECEJ niz na jednym
        # rdzeniu, a nie cztery piate.
        check("dwa procesy wolaly jadro naraz, nie po kolei (%d wobec %d "
              "tykniec)%s" % (naraz[1], po_kolei[1], etykieta),
              naraz[1] < 4 * po_kolei[1] // 5)
    else:
        check("na maszynie jednordzeniowej 'runw' nie zmienia nic "
              "(maska %d)%s" % (naraz[0], etykieta), naraz[0] == 1)
        check("i trwa tyle samo, co 'run' (%d wobec %d tykniec)%s"
              % (naraz[1], po_kolei[1], etykieta),
              naraz[1] > po_kolei[1] // 2)


def main():
    print("--- wywolania systemowe bez zamka (jadro 64-bitowe) ---")

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

    print("--- wywolania systemowe bez zamka (jadro 32-bitowe) ---")
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
