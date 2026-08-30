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
from ptest import check, summary, make_iso64, no64


def check_threads(out, suffix, ncores):
    check("proba doszla do konca" + suffix, "Watki poza" in out)
    if "Watki poza" not in out:
        return

    masks = [int(x, 16) for x in re.findall(r"watek\d: rdzenie ([0-9a-f]+)",
                                            out)]
    results = re.findall(r"watek\d: rdzenie [0-9a-f]+, wynik (\d+)", out)
    check("policzyly sie wszystkie cztery watki" + suffix,
          len(masks) == 4 and len(results) == 4)
    check("kazdy dal ten sam wynik, co porcja policzona w shellu"
          + suffix,
          "Wynik: zgadza sie" in out and len(set(results)) == 1)

    total = 0
    for m in masks:
        total |= m
    if ncores > 1:
        # Sedno: watek jadra naprawde biegl poza rdzeniem rozruchowym
        check("watki prowadzil takze rdzen inny niz rozruchowy (maska %x)%s"
              % (total, suffix),
              (total & ~1) != 0 and "Watki poza rdzeniem rozruchowym: tak"
              in out)
        core_count = bin(total).count("1")
        check("prowadzil je wiecej niz jeden rdzen (%d)%s"
              % (core_count, suffix), core_count >= 2)
    else:
        check("na jednym rdzeniu system mowi to wprost" + suffix,
              total == 1 and
              "Watki poza rdzeniem rozruchowym: NIE" in out)

    times = re.search(r"Jedna porcja: (\d+) tyknac, cztery naraz: (\d+)", out)
    check("czasy zmierzone" + suffix, times is not None)
    if not times:
        return
    one, four = int(times.group(1)), int(times.group(2))
    check("porcja odniesienia trwala dosc dlugo, by cos znaczyc (%d tyknac)%s"
          % (one, suffix), one >= 5)
    if ncores > 1:
        # Po kolei zajelyby cztery razy tyle. Prog stoi w polowie drogi
        # miedzy jednym a drugim - nie mierzymy szybkosci maszyny,
        # tylko to, czy praca w ogole idzie rownolegle.
        check("cztery porcje naraz szybciej niz po kolei (%d wobec ~%d)%s"
              % (four, 4 * one, suffix),
              four < 3 * one)
    else:
        check("na jednym rdzeniu cztery porcje trwaja jak cztery (%d wobec"
              " ~%d)%s" % (four, 4 * one, suffix),
              four >= 2 * one)


def idle_per_core(ps, ile):
    """Czy kazdy rdzen wtorny prowadzi swoje zadanie bezczynne."""
    for i in range(1, ile):
        if not re.search(r"bezczynny%d" % i, ps):
            return False
    return True


def main():
    print("--- watki jadra na wszystkich rdzeniach (jadro 64-bitowe) ---")

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

            q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, cpus=4,
                     qemu="qemu-system-x86_64")
            try:
                q.wait_serial("proton64>", timeout=90)
                ps = q.command("ps", "NAZWA", timeout=30)
                check("kazdy rdzen wtorny ma wlasne zadanie bezczynne",
                      idle_per_core(ps, 4))

                out = q.command("smpthreads", "Watki poza", timeout=180)
                check_threads(out, "", 4)

                # System ma po tym wszystkim dzialac jak dotad
                after_run = q.command("run hello64", "kod wyjscia", timeout=60)
                check("jadro dziala dalej po probie",
                      "czesc, jestem ELF-em" in after_run)
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
            ps = q3.command("ps", "NAZWA", timeout=30)
            check("kazdy rdzen wtorny ma wlasne zadanie bezczynne (32-bit)",
                  idle_per_core(ps, 4))

            out = q3.command("smpthreads", "Watki poza", timeout=180)
            check_threads(out, " (32-bit)", 4)

            after_run = q3.command("run hello", "zakonczyl sie", timeout=60)
            check("jadro dziala dalej po probie (32-bit)",
                  "zakonczyl sie (kod 0)" in after_run)
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
            out = q1.command("smpthreads", "Watki poza", timeout=180)
            check_threads(out, " (jeden rdzen)", 1)
        finally:
            q1.close()
    finally:
        if os.path.exists(slog1):
            os.unlink(slog1)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
