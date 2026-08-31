#!/usr/bin/env python3
"""Test dymny ProtonOS: pamiec wspoldzielona w jadrze 64-bitowym.

Do tej pory kazdy proces widzial wylacznie swoja pamiec i to bylo
zamierzone - o to chodzi w osobnych przestrzeniach adresowych. Pamiec
wspoldzielona jest wyjatkiem robionym swiadomie: te same ramki
fizyczne wchodza do kilku przestrzeni naraz, bo oba procesy poprosily
o ten sam klucz.

Kazdy widzi je pod SWOIM adresem - adres wirtualny jest sprawa
przestrzeni, a nie pamieci - i wlasnie to sprawdzamy najpierw, bo
gdyby adresy musialy sie zgadzac, nie byloby to zadne wspoldzielenie,
tylko przypadek.

Droga jest w obie strony: pisarz zapisuje wzorzec, czytelnik go
odczytuje i odpisuje w to samo miejsce, a pisarz widzi odpowiedz.
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


def main():
    print("--- pamiec wspoldzielona (jadro 64-bitowe) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return 0

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
            q.wait_serial("proton64>", timeout=60)
            out = q.command("run shmwrite64", "koniec, kod wyjscia",
                              timeout=40)

            check("pisarz dostal region pamieci wspoldzielonej",
                  "[shmwrite64] zapisalem 12345" in out)
            check("czytelnik - osobny proces - widzi te sama wartosc",
                  "[shmread64] wartosc=12345" in out)
            check("czytelnik potwierdza, ze to ta sama pamiec",
                  "dwa procesy, jedna pamiec" in out)
            check("odpowiedz czytelnika wraca do pisarza ta sama droga",
                  "czytelnik odpisal przez te sama pamiec" in out)
            check("oba procesy skonczyly sie bez bledu",
                  "koniec, kod wyjscia 0" in out and "ZLE" not in out)

            # Region lezy w obszarze mapowan (od 0x50000000 w gore),
            # a nie w stercie ani na stosie - i to widac po adresie
            adresy = re.findall(r"region pod adresem (\d+)", out)
            check("oba procesy dostaly adres z obszaru mapowan",
                  len(adresy) == 2 and
                  all(0x50000000 <= int(a) < 0x60000000 for a in adresy))
            # Pisarz zmapowal wczesniej region na boku, wiec wspolny
            # widzi wyzej niz czytelnik. Te same bajty pod dwoma
            # roznymi adresami - o to w tym chodzi.
            check("kazdy widzi te sama pamiec pod SWOIM adresem",
                  len(adresy) == 2 and adresy[0] != adresy[1])
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
