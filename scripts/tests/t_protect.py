#!/usr/bin/env python3
"""Test dymny ProtonOS: granica miedzy jadrem a programem.

Program w pierscieniu 3 nie siegnie po pamiec jadra sam - pilnuje tego
stronicowanie. Moze to jednak zrobic cudzymi rekami: podac w wywolaniu
systemowym wskaznik na pamiec jadra i liczyc na to, ze jadro zapisze
tam cos w swoim imieniu, z pelnymi prawami.

Dlatego sprawdzamy obie drogi. 'evil' siega wprost i ma zginac,
a jadro ma pokazac adres, po ktory siegnal. 'evil2' podaje takie
wskazniki w syscallach - kazdy ma wrocic z bledem, ale sam proces ma to
PRZEZYC i wyjsc z kodem zero. Odrzucenie, ktore zabija proces, tez by
tu przeszlo, a nie o to chodzi.

Uruchamiany osobno albo przez scripts/run_tests.py, ktory puszcza
wszystkie grupy rownolegle.
"""

import os
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
        q.wait_for("[siec] DHCP", timeout=20)
        time.sleep(0.5)

        # 'run evil': proba czytania pamieci jadra wprost
        q.type_line("clear")
        q.type_line("run evil")
        screen = q.wait_for("proces 'evil' ubity")
        check("evil czytal pamiec jadra i zostal ubity",
              "proces 'evil' ubity" in screen)
        check("CR2 pokazuje atakowany adres",
              "siegnal po adres 100000" in screen)
        check("kod po ataku nie wykonal sie",
              "TEGO NIE POWINNO BYC WIDAC" not in screen)

        # 'run evil2': te same adresy, ale podane jadru w syscallach
        q.type_line("run evil2")
        ev2 = q.wait_for("granica trzyma", timeout=20)
        check("syscalle odrzucaja wskazniki na pamiec jadra",
              "jadro odrzucilo wszystkie proby" in ev2)
        check("evil2 nie wypisal pamieci jadra na konsole",
              "PRZESZLO" not in ev2 and "DZIURA" not in ev2)
        check("proces przezyl odrzucone syscalle (kod 0)",
              "kod 0" in ev2)
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
