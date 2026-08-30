#!/usr/bin/env python3
"""Test dymny ProtonOS: zapis przez plik odwzorowany w pamieci.

Odwzorowania byly dotad jednokierunkowe: plik dawalo sie czytac jak
pamiec, ale nie pisac. Teraz droga prowadzi w obie strony - program
zmienia bajty pod adresem, a one trafiaja do pliku na dysku.

Sprawdzamy przy tym cztery rzeczy, ktore latwo zrobic zle:

  1. Odsylamy TYLKO to, co sie zmienilo. Program czyta caly plik,
     a pisze po dwoch stronach - i dwie strony maja wrocic na dysk.
     O tym, ktore, mowi procesor: bit "brudna" ustawia sam przy
     zapisie. Drugie sys_msync z rzedu ma odeslac zero.
  2. Zmiany widac ZWYKLYM odczytem, a po restarcie maszyny takze na
     dysku - nie tylko w pamieci, ktora je trzymala.
  3. Program nie musi prosic: to, czego nie zdazyl odeslac sam, odsyla
     jadro przy koncu procesu.
  4. Zapisywalne odwzorowanie po fork-u jest WSPOLNE, a nie kopiowane
     przy zapisie. Oba procesy pisza do jednego pliku, wiec prywatna
     kopia u dziecka znaczylaby, ze jego zapisy nigdy nie wychodza
     poza jego wlasna pamiec.

Oba jadra maja to samo, kazde swoim kodem stronicowania - i oba
sprawdzamy tak samo.

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


def main():
    print("--- zapis przez odwzorowanie (jadro 64-bitowe) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
    else:
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

                make_it = q.command("run mkmap64 zapis.txt 12", "kod wyjscia",
                                   timeout=60)
                check("plik z wzorcem zalozony na dysku",
                      "[mkmap64] zapisano 12288 B wzorca" in make_it)

                write_it = q.command("run mmapzap64 zapis.txt pisz",
                                   "kod wyjscia", timeout=90)
                check("caly plik da sie przeczytac przez odwzorowanie "
                      "do zapisu",
                      "stary wzorzec przeczytany, bajtow 12288" in write_it)
                check("odsylane sa tylko strony, do ktorych pisano",
                      "odeslanych stron: 2" in write_it)
                check("odeslana strona przestaje byc brudna",
                      "drugie msync odeslalo: 0" in write_it)
                check("odeslanie na dysk nie rusza tresci w pamieci",
                      "zapis przez pamiec gotowy" in write_it and "ZLE" not in write_it)

                probe = q.command("run mmapzap64 zapis.txt spr",
                                  "kod wyjscia", timeout=90)
                check("zwykly odczyt widzi to, co wpisala pamiec",
                      "zwykly odczyt widzi to" in probe and "ZLE" not in probe)

                # Bez jawnego msync - odsyla jadro na koncu procesu
                q.command("run mkmap64 bezsync.txt 12", "kod wyjscia",
                            timeout=60)
                without = q.command("run mmapzap64 bezsync.txt bez",
                                  "kod wyjscia", timeout=60)
                check("program moze skonczyc sie bez proszenia o odeslanie",
                      "koncze bez msync" in without)
                probe2 = q.command("run mmapzap64 bezsync.txt spr2",
                                   "kod wyjscia", timeout=60)
                check("jadro odsyla brudne strony przy koncu procesu",
                      "doszly na dysk przy koncu procesu" in probe2)

                # Po fork-u odwzorowanie jest wspolne, a nie kopiowane
                q.command("run mkmap64 wspolny.txt 12", "kod wyjscia",
                            timeout=60)
                visible = q.command("run mmapzap64 wspolny.txt widac",
                                    "kod wyjscia", timeout=60)
                check("rodzic widzi zapis dziecka - odwzorowanie wspolne",
                      "odwzorowanie wspolne" in visible and "ZLE" not in visible)
            finally:
                q.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)
            for f in (iso, slog, disk):
                if os.path.exists(f):
                    os.unlink(f)

    # --- To samo w jadrze 32-bitowym, z restartem na koniec ---
    print("--- zapis przez odwzorowanie (jadro 32-bitowe) ---")
    disk32 = tempfile.mktemp(suffix=".img")
    slog32 = tempfile.mktemp(suffix=".log")
    slog4 = tempfile.mktemp(suffix=".log")   # drugi rozruch pisze na swoj
    with open(disk32, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        q3 = Qemu(KERNEL, INITRD, disk32, append=TEXT_MODE, serial=slog32)
        try:
            q3.wait_for("ProtonOS shell", timeout=40)
            q3.wait_for("[siec] DHCP", timeout=30)

            make_it = q3.command("run mkmap zapis.txt 12", "zakonczyl sie",
                                timeout=60)
            check("plik z wzorcem zalozony na dysku (32-bit)",
                  "[mkmap] zapisano 12288 B wzorca" in make_it)

            write_it = q3.command("run mmapzap zapis.txt pisz", "zakonczyl sie",
                                timeout=90)
            check("caly plik da sie przeczytac przez odwzorowanie "
                  "do zapisu (32-bit)",
                  "stary wzorzec przeczytany, bajtow 12288" in write_it)
            check("odsylane sa tylko strony, do ktorych pisano (32-bit)",
                  "odeslanych stron: 2" in write_it)
            check("odeslana strona przestaje byc brudna (32-bit)",
                  "drugie msync odeslalo: 0" in write_it)
            check("odeslanie na dysk nie rusza tresci w pamieci (32-bit)",
                  "zapis przez pamiec gotowy" in write_it and "ZLE" not in write_it)

            probe = q3.command("run mmapzap zapis.txt spr", "zakonczyl sie",
                               timeout=90)
            check("zwykly odczyt widzi to, co wpisala pamiec (32-bit)",
                  "zwykly odczyt widzi to" in probe and "ZLE" not in probe)

            q3.command("run mkmap bezsync.txt 12", "zakonczyl sie",
                         timeout=60)
            without = q3.command("run mmapzap bezsync.txt bez", "zakonczyl sie",
                               timeout=60)
            check("program moze skonczyc sie bez proszenia o odeslanie "
                  "(32-bit)", "koncze bez msync" in without)
            probe2 = q3.command("run mmapzap bezsync.txt spr2",
                                "zakonczyl sie", timeout=60)
            check("jadro odsyla brudne strony przy koncu procesu (32-bit)",
                  "doszly na dysk przy koncu procesu" in probe2)

            q3.command("run mkmap wspolny.txt 12", "zakonczyl sie",
                         timeout=60)
            visible = q3.command("run mmapzap wspolny.txt widac",
                                 "zakonczyl sie", timeout=60)
            check("rodzic widzi zapis dziecka - odwzorowanie wspolne "
                  "(32-bit)",
                  "odwzorowanie wspolne" in visible and "ZLE" not in visible)
        finally:
            q3.close()

        # Najmocniejszy dowod: TEN SAM obraz dysku, nowe jadro, nowa
        # pamiec. Gdyby zmiany siedzialy tylko w stronach procesu,
        # tutaj nie byloby po nich sladu.
        q4 = Qemu(KERNEL, INITRD, disk32, append=TEXT_MODE, serial=slog4)
        try:
            screen_text = q4.wait_for("ProtonOS shell", timeout=40)
            check("drugi rozruch na tym samym dysku (bez formatowania)",
                  "+ PFS gotowy" in screen_text and "sformatowany" not in screen_text)
            q4.wait_for("[siec] DHCP", timeout=30)

            po = q4.command("run mmapzap zapis.txt spr", "zakonczyl sie",
                              timeout=90)
            check("zapisy przez pamiec przetrwaly restart maszyny",
                  "zwykly odczyt widzi to" in po and "ZLE" not in po)
            po2 = q4.command("run mmapzap bezsync.txt spr2",
                               "zakonczyl sie", timeout=60)
            check("bajty odeslane przy koncu procesu tez przetrwaly restart",
                  "doszly na dysk przy koncu procesu" in po2)
        finally:
            q4.close()
    finally:
        for f in (disk32, slog32, slog4):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
