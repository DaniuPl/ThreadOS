#!/usr/bin/env python3
"""Test dymny ProtonOS: komendy shella jadra.

Shell jadra jest jedynym wejsciem do systemu, dopoki nie ruszy ush
w pierscieniu 3 - i pierwszym, ktore przestaje dzialac, gdy cos sie
zepsuje glebiej. Sprawdzamy, ze wypisuje to, co ma, i ze liczby, ktore
pokazuje, zgadzaja sie z rzeczywistoscia.

Najmocniejsza asercja jest tu ta o pamieci zarzadzanej: mapowanie
identycznosciowe ma siegac az po szczyt wykrytego RAM-u, a nie po
pierwsze 64 MiB.

Uruchamiany osobno albo przez scripts/run_tests.py, ktory puszcza
wszystkie grupy rownolegle.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    slog = tempfile.mktemp(suffix=".log")

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
    try:
        q.wait_for("ProtonOS shell")
        # Komunikat DHCP wchodzi w srodek pisanej linii, wiec czekamy
        # na niego, zanim zaczniemy pisac
        q.wait_for("[siec] DHCP", timeout=20)
        time.sleep(0.5)

        q.type_line("help")
        screen = q.screen()
        check("komenda 'help' wypisuje liste komend",
              "Dostepne komendy" in screen and "reboot" in screen)

        q.type_line("mem")
        screen = q.screen()
        check("komenda 'mem' pokazuje RAM i ramki",
              "RAM wykryty" in screen and "ramek 4 KiB" in screen)
        check("komenda 'mem' pokazuje sterte", "sterta jadra" in screen)
        # Cala wykryta pamiec jest pod zarzadem, nie tylko pierwsze
        # 64 MiB: mapowanie identycznosciowe ciagnie sie az po szczyt
        # RAM-u.
        mm = re.search(r"zarzadzane (\d+) MiB", screen)
        ram = re.search(r"RAM wykryty:\s+(\d+) KiB", screen)
        check("zarzadzana pamiec siega szczytu wykrytego RAM-u"
              + (f" ({mm.group(1)} MiB z {int(ram.group(1)) // 1024} MiB)"
                 if mm and ram else ""),
              mm is not None and ram is not None
              and int(mm.group(1)) >= int(ram.group(1)) // 1024 - 2)

        q.type_line("uptime")
        check("komenda 'uptime' dziala", "tyknac timera" in q.screen())

        q.type_line("ps")
        screen = q.screen()
        check("komenda 'ps' pokazuje watki",
              "main" in screen and "demo-A" in screen
              and "demo-B" in screen)
        check("shell jest wlasnym watkiem (nie kontekst przerwania)",
              "shell (biezacy)" in screen)
        check("zombie po watku C zostal sprzatniety",
              "demo-C" not in screen)
        check("ps pokazuje rodzica kazdego watku",
              "RODZIC" in screen
              and re.search(r"\n  0   -", screen) is not None)

        # initrd: 'ls' pokazuje spakowane programy. Plikow jest wiecej,
        # niz miesci sie na ekranie, wiec wczesne sie przewijaja -
        # i przewija ich sie tym wiecej, im wiecej programow przybywa.
        # Pytamy wiec o KSZTALT listy, a nie o konkretne nazwy: kazdy
        # wiersz to nazwa i niezerowy rozmiar, wierszy jest kilkanascie,
        # a wsrod nich znajome programy.
        q.type_line("ls")
        screen = q.screen()
        entries = re.findall(r"(\w+)\s+\((\d+) B\)", screen)
        known = {"forktest", "ptree", "sigtest", "lazymem", "swaptest",
                 "mkmap", "mmapfile", "deep", "mmapzap", "dynlib"}
        check("komenda 'ls' pokazuje pliki initrd (%d wpisow, %d znanych)"
              % (len(entries), len({n for n, _ in entries} & known)),
              len(entries) >= 15
              and all(int(size) > 0 for _, size in entries)
              and len({n for n, _ in entries} & known) >= 3)

        q.type_line("run brak")
        check("run zglasza brak pliku - i szuka go w obu miejscach",
              "nie ma 'brak' ani w initrd, ani na dysku" in q.screen())
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
