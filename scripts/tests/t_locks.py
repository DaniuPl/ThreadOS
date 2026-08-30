#!/usr/bin/env python3
"""Test dymny ProtonOS: zamki na wspolnych strukturach jadra.

Rdzenie wtorne umialy dotad tylko liczyc na tym, co dostaly - caly
dzielony miedzy nimi stan to byla jedna liczba i jeden zamek. Zeby
mogly kiedykolwiek prowadzic zadania, musza najpierw umiec bezpiecznie
WOLAC JADRO; a jadro musi to wytrzymac.

Komenda 'smpmem' bierze dwie struktury, po ktore siega wszystko inne -
mape ramek fizycznych i sterte jadra - i kaze kazdemu rdzeniowi
przydzielac z nich i oddawac trzy tysiace razy naraz.

Sprawdzenie nie polega na tym, czy sie nie wysypalo: na to odpowiedz
bywa przypadkowa. Kazdy przydzial dostaje wzorzec zalezny od numeru
jednostki i zaraz potem jest czytany - ramka wydana dwom rdzeniom
naraz ma dwoch piszacych i wzorzec sie nie zgadza. Na koniec liczymy
ramki i bajty sterty: tyle samo przed co po.

Ta proba naprawde cos rozstrzyga. Bez zamka w mapie ramek daje
kilkadziesiat niezgodnosci wzorca i kilkadziesiat ramek, ktore nie
wrocily; bez zamka na stercie jadro ginie na bledzie strony, bo lista
wolnych blokow przestaje trzymac sie kupy.

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


def verify(out, suffix, ncores):
    check("proba doszla do konca" + suffix, "Liczylo rdzeni:" in out)
    if "Liczylo rdzeni:" not in out:
        return

    m = re.search(r"Przydzialow: (\d+), nieudanych: (\d+)", out)
    check("wszystkie przydzialy sie udaly" + suffix,
          m is not None and int(m.group(1)) == 64 * 48 and
          int(m.group(2)) == 0)

    # Sedno: ramka wydana dwa razy ma dwoch piszacych
    check("kazdy przydzial trafil do jednego rdzenia" + suffix,
          "Wzorzec: zgadza sie (bledow 0)" in out)

    ram = re.search(r"Ramki: (\d+) -> (\d+), sterta: (\d+) -> (\d+) B", out)
    check("mapa ramek i sterta wrocily do stanu sprzed proby" + suffix,
          ram is not None and ram.group(1) == ram.group(2) and
          ram.group(3) == ram.group(4))
    check("jadro samo mowi, ze nic nie wycieklo" + suffix,
          "Nic nie wycieklo: tak" in out)

    check("maszyna widzi %d %s%s"
          % (ncores, "rdzen" if ncores == 1 else "rdzenie", suffix),
          re.search(r"Rdzeni: %d \(obudzonych %d\)" % (ncores, ncores - 1),
                    out) is not None)

    # Ile rdzeni naprawde wzielo prace. Nie zadamy, zeby wzial ja KAZDY:
    # rdzen rozruchowy prowadzi w tym czasie watek shella i bywa
    # wywlaszczony przez timer, podczas gdy wtorne kreca sie w ciasnej
    # petli i rozbieraja zlecenie, zanim zdazy siegnac. To wyscig,
    # a wyscigi nie sa asercjami.
    share = [int(v) for v in re.findall(r"rdzen\d+=(\d+)", out)]
    check(("caly zestaw policzyl jedyny rdzen (%s)%s" if ncores == 1
           else "prace wzielo wiecej niz jeden rdzen (%s)%s")
          % (share, suffix),
          len(share) == ncores and
          (ncores == 1 or sum(1 for v in share if v) >= 2))

    # To jest wlasciwe pytanie o wspolbieznosc: nie "czy cos sie
    # policzylo", tylko czy policzylo sie DOKLADNIE RAZ KAZDE
    check("wszystkie 64 jednostki policzone dokladnie raz" + suffix,
          sum(share) == 64)


def main():
    print("--- zamki w jadrze 64-bitowym (cztery rdzenie) ---")

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
                out = q.command("smpmem", "Liczylo rdzeni", timeout=120)
                verify(out, "", 4)

                # System ma po tym wszystkim zyc dalej: gdyby zamki
                # zostawily po sobie polamana sterte, nastepny program
                # nie mialby z czego powstac
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

    print("--- zamki w jadrze 32-bitowym (cztery rdzenie) ---")
    slog32 = tempfile.mktemp(suffix=".log")
    try:
        q3 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog32, cpus=4)
        try:
            q3.wait_for("ProtonOS shell", timeout=40)
            out = q3.command("smpmem", "Liczylo rdzeni", timeout=120)
            verify(out, " (32-bit)", 4)
            after_run = q3.command("run hello", "zakonczyl sie", timeout=60)
            check("jadro dziala dalej po probie (32-bit)",
                  "zakonczyl sie (kod 0)" in after_run)
        finally:
            q3.close()
    finally:
        if os.path.exists(slog32):
            os.unlink(slog32)

    # --- Maszyna jednordzeniowa ---
    # Ten sam kod, jeden rdzen: zamek ma byc nieszkodliwy tam, gdzie
    # nie ma z kim rywalizowac
    print("--- ta sama proba na jednym rdzeniu ---")
    slog1 = tempfile.mktemp(suffix=".log")
    try:
        q1 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog1)
        try:
            q1.wait_for("ProtonOS shell", timeout=40)
            out = q1.command("smpmem", "Liczylo rdzeni", timeout=120)
            verify(out, " (jeden rdzen)", 1)
        finally:
            q1.close()
    finally:
        if os.path.exists(slog1):
            os.unlink(slog1)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
