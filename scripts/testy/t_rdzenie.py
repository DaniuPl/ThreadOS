#!/usr/bin/env python3
"""Test dymny ProtonOS: watki jadra prowadzone przez wszystkie rdzenie.

To jest ta zmiana, na ktora czekaly poprzednie. Rdzen wtorny mial juz
tablice wyjatkow, wlasny zegar i zamki na wspolnych strukturach - ale
biezace zadanie bylo wciaz JEDNA zmienna globalna, i dopoki nia bylo,
wiecej niz jeden rdzen nie mial jak prowadzic zadan. Teraz jest ich
tyle, ile rdzeni.

Sprawdzamy trzy rzeczy, bo kazda z osobna dalaby sie podrobic:

  1. Kazdy rdzen wtorny ma wlasne zadanie bezczynne i prowadzi je -
     widac to w 'ps'. Bez tego nie byloby dokad odlozyc jego kontekstu.
  2. Watek sam zapisuje, na ktorym rdzeniu wlasnie liczy. Gdyby jednak
     scheduler tylko przerzucal go miedzy rdzeniami po kolei, maska
     wygladalaby tak samo.
  3. Dlatego mierzymy CZAS. Cztery porcje puszczone rownolegle maja
     zajac wyraznie mniej niz cztery razy tyle, co jedna. Jednego
     rdzenia nie da sie o to oszukac: on musi policzyc je po kolei.

Wynik kazdej porcji jest ten sam, co porcji policzonej w watku shella -
co do bitu. Watek, ktory policzyl swoje na innym rdzeniu, ma dac
dokladnie ta sama liczbe.

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


def sprawdz_watki(out, dopisek, rdzeni):
    check("proba doszla do konca" + dopisek, "Watki poza" in out)
    if "Watki poza" not in out:
        return

    maski = [int(x, 16) for x in re.findall(r"watek\d: rdzenie ([0-9a-f]+)",
                                            out)]
    wyniki = re.findall(r"watek\d: rdzenie [0-9a-f]+, wynik (\d+)", out)
    check("policzyly sie wszystkie cztery watki" + dopisek,
          len(maski) == 4 and len(wyniki) == 4)
    check("kazdy dal ten sam wynik, co porcja policzona w shellu"
          + dopisek,
          "Wynik: zgadza sie" in out and len(set(wyniki)) == 1)

    razem = 0
    for m in maski:
        razem |= m
    if rdzeni > 1:
        # Sedno: watek jadra naprawde biegl poza rdzeniem rozruchowym
        check("watki prowadzil takze rdzen inny niz rozruchowy (maska %x)%s"
              % (razem, dopisek),
              (razem & ~1) != 0 and "Watki poza rdzeniem rozruchowym: tak"
              in out)
        ile_rdzeni = bin(razem).count("1")
        check("prowadzil je wiecej niz jeden rdzen (%d)%s"
              % (ile_rdzeni, dopisek), ile_rdzeni >= 2)
    else:
        check("na jednym rdzeniu system mowi to wprost" + dopisek,
              razem == 1 and
              "Watki poza rdzeniem rozruchowym: NIE" in out)

    czasy = re.search(r"Jedna porcja: (\d+) tyknac, cztery naraz: (\d+)", out)
    check("czasy zmierzone" + dopisek, czasy is not None)
    if not czasy:
        return
    jedna, cztery = int(czasy.group(1)), int(czasy.group(2))
    check("porcja odniesienia trwala dosc dlugo, by cos znaczyc (%d tyknac)%s"
          % (jedna, dopisek), jedna >= 5)
    if rdzeni > 1:
        # Po kolei zajelyby cztery razy tyle. Prog stoi w polowie drogi
        # miedzy jednym a drugim - nie mierzymy szybkosci maszyny,
        # tylko to, czy praca w ogole idzie rownolegle.
        check("cztery porcje naraz szybciej niz po kolei (%d wobec ~%d)%s"
              % (cztery, 4 * jedna, dopisek),
              cztery < 3 * jedna)
    else:
        check("na jednym rdzeniu cztery porcje trwaja jak cztery (%d wobec"
              " ~%d)%s" % (cztery, 4 * jedna, dopisek),
              cztery >= 2 * jedna)


def bezczynne_rdzeni(ps, ile):
    """Czy kazdy rdzen wtorny prowadzi swoje zadanie bezczynne."""
    for i in range(1, ile):
        if not re.search(r"bezczynny%d" % i, ps):
            return False
    return True


def main():
    print("--- watki jadra na wszystkich rdzeniach (jadro 64-bitowe) ---")

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
                ps = q.polecenie("ps", "NAZWA", timeout=30)
                check("kazdy rdzen wtorny ma wlasne zadanie bezczynne",
                      bezczynne_rdzeni(ps, 4))

                out = q.polecenie("smpwatki", "Watki poza", timeout=180)
                sprawdz_watki(out, "", 4)

                # System ma po tym wszystkim dzialac jak dotad
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

    print("--- watki jadra na wszystkich rdzeniach (jadro 32-bitowe) ---")
    slog32 = tempfile.mktemp(suffix=".log")
    try:
        q3 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog32, cpus=4)
        try:
            q3.wait_for("ProtonOS shell", timeout=40)
            ps = q3.polecenie("ps", "NAZWA", timeout=30)
            check("kazdy rdzen wtorny ma wlasne zadanie bezczynne (32-bit)",
                  bezczynne_rdzeni(ps, 4))

            out = q3.polecenie("smpwatki", "Watki poza", timeout=180)
            sprawdz_watki(out, " (32-bit)", 4)

            dalej = q3.polecenie("run hello", "zakonczyl sie", timeout=60)
            check("jadro dziala dalej po probie (32-bit)",
                  "zakonczyl sie (kod 0)" in dalej)
        finally:
            q3.close()
    finally:
        if os.path.exists(slog32):
            os.unlink(slog32)

    # --- Maszyna jednordzeniowa ---
    # Ten sam kod, jeden rdzen: watki maja sie policzyc tak samo,
    # tyle ze po kolei
    print("--- ta sama proba na jednym rdzeniu ---")
    slog1 = tempfile.mktemp(suffix=".log")
    try:
        q1 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog1)
        try:
            q1.wait_for("ProtonOS shell", timeout=40)
            out = q1.polecenie("smpwatki", "Watki poza", timeout=180)
            sprawdz_watki(out, " (jeden rdzen)", 1)
        finally:
            q1.close()
    finally:
        if os.path.exists(slog1):
            os.unlink(slog1)

    return wynik()


if __name__ == "__main__":
    sys.exit(main())
