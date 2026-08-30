#!/usr/bin/env python3
"""Test dymny ProtonOS: rozruch jadra 32-bitowego.

Pierwsza z grup wydzielonych z dawnego zbiorczego 'rdzen'. Pyta o to,
co dzieje sie ZANIM ktokolwiek cokolwiek wpisze: czy jadro doszlo do
konca inicjalizacji, czy rozpoznalo sprzet, czy wystartowal timer,
scheduler i siec.

Kolejnosc komunikatow czytamy z portu szeregowego, a nie z ekranu -
z ekranu najstarsze wiersze wyjezdzaja, a dowod, ktory znika, gdy
maszyna zwolni, nie jest zadnym dowodem.

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
        screen = q.wait_for("ProtonOS shell")
        print("=== Ekran VGA po boocie ===")
        print(screen)
        print("===========================")

        check("dysk + PFS wykryty i sformatowany",
              "+ PFS gotowy" in screen)
        check("kontroler dysku nazwany przy boocie (tu stary ATA PIO)",
              "Dysk ATA PIO + PFS gotowy" in screen)
        check("mysz PS/2 wykryta przy boocie",
              "Mysz PS/2 gotowa" in screen)
        check("karta sieciowa RTL8139 wykryta przy boocie",
              "Siec: RTL8139" in screen)
        check("na jednym rdzeniu system mowi to wprost",
              "Procesory: 1" in q.serial())

        # Adres IP przychodzi z serwera DHCP slirpa juz po starcie
        # shella. Czekamy na komunikat, zanim ekran zacznie sluzyc
        # testom klawiatury - inaczej wpada w srodek pisanej linii.
        q.wait_for("[siec] DHCP", timeout=20)
        # Komunikat sklada sie z kilku printk-ow, wiec chwila na
        # dopisanie reszty linii - inaczej test czyta ja w polowie
        time.sleep(0.5)
        lease = q.screen()
        check("DHCP przyznal adres IP", "przyznano 10.0.2.15" in lease)
        check("DHCP przekazal brame i serwer DNS",
              "brama 10.0.2.2" in lease and "DNS 10.0.2.3" in lease)

        log_text = q.serial()
        for expected in ["Multiboot OK", "GDT zaladowana", "IDT zaladowana",
                         "PIC przemapowany", "Timer PIT", "Klawiatura PS/2",
                         "Przerwania wlaczone"]:
            check(f"komunikat '{expected}'", expected in log_text)

        check("obsluga wyjatku int3 (breakpoint)", "[INT3]" in log_text)

        for expected in ["Pamiec fizyczna", "Stronicowanie wlaczone",
                         "kmalloc/kfree dzialaja"]:
            check(f"komunikat '{expected}'", expected in screen)

        check("jadro przezylo init pamieci (doszlo do shella)",
              "ProtonOS shell" in screen)

        # Watek C konczy sie po dwoch sekundach. Jego napis w wierszu
        # zerowym znika przy kazdym przewinieciu ekranu, wiec pytamy
        # o to port szeregowy: tam nic nie wyjezdza.
        end_c = q.wait_serial("Watek demonstracyjny C zakonczyl prace",
                              timeout=30)
        check("watek C zakonczyl prace",
              "Watek demonstracyjny C zakonczyl prace" in end_c)

        # Timer: uptime w rogu ekranu musi urosnac
        def uptime(s):
            m = re.search(r"uptime: (\d+)s", s)
            return int(m.group(1)) if m else None

        t1 = uptime(q.wait_for("uptime: "))
        time.sleep(2)
        t2 = uptime(q.screen())
        check(f"timer PIT tyka (uptime {t1}s -> {t2}s)",
              t1 is not None and t2 is not None and t2 > t1)

        # Wielozadaniowosc: liczniki watkow A i B musza rosnac
        # niezaleznie. Bierzemy OSTATNIE wystapienie - po przewinieciu
        # ekranu wyzej moga wisiec stare, martwe kopie linii.
        def counters(s):
            a = re.findall(r"watek-A: (\d+)", s)
            b = re.findall(r"watek-B: (\d+)", s)
            return (int(a[-1]) if a else None, int(b[-1]) if b else None)

        a1, b1 = counters(q.wait_for("watek-A: "))
        time.sleep(1.5)
        a2, b2 = counters(q.screen())
        check(f"watek A liczy ({a1} -> {a2})",
              a1 is not None and a2 is not None and a2 > a1)
        check(f"watek B liczy ({b1} -> {b2})",
              b1 is not None and b2 is not None and b2 > b1)

        # Klawiatura: wstrzyknij "proton 1" i sprawdz echo
        for key in ["p", "r", "o", "t", "o", "n", "spc", "1"]:
            q.sendkey(key)
        q.sendkey("shift-x")  # test shifta: powinno wyjsc 'X'
        screen = q.screen()
        check("echo klawiatury: 'proton 1'", "> proton 1" in screen)
        check("echo klawiatury z shiftem: 'X'", "> proton 1X" in screen)

        q.sendkey("ret")
        time.sleep(0.3)
        check("nieznana komenda zgloszona",
              "nieznana komenda: 'proton'" in q.screen())
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
