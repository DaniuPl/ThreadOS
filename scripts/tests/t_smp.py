#!/usr/bin/env python3
"""Test dymny ProtonOS: wykrycie i wybudzenie pozostalych procesorow.

Po wlaczeniu zasilania pracuje jeden rdzen - ten, ktory firmware
wybral na rozruchowy. Reszta stoi i czeka na sygnal, i to jest decyzja
sprzetu, nie systemu: procesor obudzony sygnalem SIPI zaczyna w trybie
rzeczywistym, szesnastobitowo, pod adresem (wektor << 12) - dokladnie
tak, jak zaczynal pecet czterdziesci lat temu.

To jest pierwszy krok, nie caly SMP. Obudzony rdzen przechodzi wlasna
droga do trybu chronionego, zglasza sie i staje. Dawanie mu pracy
wymaga schedulera per rdzen i zamkow w kazdej strukturze jadra, a te
pisane byly dla jednego rdzenia i cicho na to licza.

Rdzenie wtorne nie prowadza wlasnych zadan - scheduler i lista
procesow pisane byly dla jednego rdzenia. Dostaja za to prace
policzalna: wspolna kolejke jednostek, z ktorej kazdy bierze kolejny
numer pod zamkiem. To jest caly stan dzielony miedzy rdzeniami - jedna
liczba i jeden zamek.

Wlasciwe pytanie o wspolbieznosc nie brzmi "czy cos sie policzylo",
tylko "czy policzylo sie DOKLADNIE RAZ kazde". Dlatego wynik pracy
rozdzielonej porownujemy z policzonym na jednym rdzeniu: zgadza sie
tylko wtedy, gdy zadna jednostka nie przepadla ani nie zostala
policzona dwa razy, i nikt nie zadeptal cudzej komorki.

Test sprawdza tez, ze system po tym wszystkim dziala dokladnie tak
samo. Samo "widze cztery procesory" nie byloby nic warte, gdyby przy
okazji cos w systemie pekalo.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def values(text, label):
    """Liczby z JEDNEGO wiersza wypisu 'smp'.

    Komenda podaje teraz dwa wiersze w tym samym ksztalcie - udzial
    w pracy i tykniecia wlasnych zegarow - wiec szukanie 'rdzenN=' po
    calym tekscie zlepialoby jedno z drugim."""
    for w in text.replace("\r", "").split("\n"):
        if w.startswith(label):
            return [int(x) for x in re.findall(r"rdzen\d+=(\d+)", w)]
    return []


def main():
    print("--- pozostale procesory (INIT-SIPI) ---")

    # --- Cztery rdzenie ---
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog, cpus=4)
    try:
        start = q.wait_for("Procesory:", timeout=60)
        m = re.search(r"Procesory: (\d+) \(obudzonych (\d+)", start)
        check("system wylicza wszystkie procesory z tablicy ACPI",
              m is not None and int(m.group(1)) == 4)
        check("trzy pozostale rdzenie zglosily sie po INIT-SIPI",
              m is not None and int(m.group(2)) == 3)

        q.wait_for("ProtonOS shell", timeout=30)
        q.wait_for("[siec] DHCP", timeout=30)

        # System po obudzeniu rdzeni ma dzialac jak dotad
        up1 = q.command("uptime", "tyknac timera")
        check("timer tyka po obudzeniu pozostalych rdzeni",
              "tyknac timera" in up1)

        proc = q.command("run hello", "zakonczyl sie", timeout=40)
        check("proces w ring 3 dziala jak dotad",
              "[hello] czesc, jestem procesem ELF" in proc)

        files = q.command("write smptest wielordzeniowo", "zapisano")
        check("dysk dziala jak dotad", "zapisano" in files)
        read_res = q.command("cat smptest", "wielordzeniowo")
        check("i oddaje to, co zapisano", "wielordzeniowo" in read_res)

        # Praca rozdzielona na rdzenie: wynik ma sie zgadzac co do bitu
        # z policzonym na jednym rdzeniu
        work = q.command("smp", "Liczylo rdzeni", timeout=90)
        check("wynik pracy rozdzielonej zgadza sie z jednordzeniowym",
              "zgadza sie" in work and "NIE ZGADZA SIE" not in work)

        m = re.search(r"Liczylo rdzeni: (\d+)", work)
        check("prace liczylo kilka rdzeni naraz",
              m is not None and int(m.group(1)) >= 2)

        # Ile rdzeni naprawde wzielo prace. Nie zadamy, zeby wzial ja
        # KAZDY: rdzen bezczynny stoi na instrukcji hlt i budzi go
        # dopiero przerwanie, a jak szybko dojdzie, zalezy od maszyny
        # wirtualnej pod spodem. To wyscig, a wyscigi nie sa asercjami.
        shares = values(work, "Udzial:")
        check("prace wzielo wiecej niz jeden rdzen (%s)" % shares,
              len(shares) == 4 and sum(1 for u in shares if u) >= 2)
        check("razem policzono dokladnie tyle jednostek, ile zlecono",
              sum(shares) == 64)

        # --- Wlasny zegar kazdego rdzenia -------------------------
        # Rdzen wtorny nie moze korzystac z PIT: to przerwanie idzie
        # przez kontroler PIC do JEDNEGO procesora. Ma za to wlasny
        # zegar w lokalnym APIC - i dopiero on daje mu przerwania,
        # bez ktorych nigdy nie dalby sie wywlaszczyc.
        clocks = values(work, "Zegary rdzeni:")
        check("kazdy rdzen wtorny ma wlasny zegar, ktory tyka (%s)"
              % clocks,
              len(clocks) == 4 and all(z > 0 for z in clocks[1:]))
        check("rdzen rozruchowy chodzi z PIT, nie z APIC",
              len(clocks) == 4 and clocks[0] == 0)

        # Jedno odczytanie niczego nie dowodzi - liczniki maja ROSNAC
        time.sleep(1.5)
        again = values(q.command("smp", "Zegary rdzeni", timeout=90),
                         "Zegary rdzeni:")
        check("zegary rdzeni wtornych ida dalej (%s -> %s)"
              % (clocks, again),
              len(again) == 4 and
              all(again[i] > clocks[i] for i in range(1, 4)))

        # Liczniki watkow rosna dalej - scheduler nie stanal
        def counter():
            m = re.findall(r"watek-A: (\d+)", q.screen())
            return int(m[-1]) if m else None

        a1 = counter()
        time.sleep(1.5)
        a2 = counter()
        check("scheduler dziala dalej (%s -> %s)" % (a1, a2),
              a1 is not None and a2 is not None and a2 > a1)
    finally:
        q.close()
        for f in (slog, disk):
            if os.path.exists(f):
                os.unlink(f)

    # --- Tryb dlugi ---
    # Rdzen wtorny startuje w trybie rzeczywistym takze wtedy, gdy
    # reszta systemu pracuje od dawna 64-bitowo: musi przejsc sam cala
    # droge, szesnascie bitow -> trzydziesci dwa -> szescdziesiat
    # cztery. Tablice stron bierze gotowe, te same, co jadro.
    print("--- pozostale procesory w trybie dlugim ---")
    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
    else:
        d = tempfile.mkdtemp()
        iso = tempfile.mktemp(suffix=".iso")
        slog64 = tempfile.mktemp(suffix=".log")
        try:
            check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
            if os.path.exists(iso):
                q64 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog64, cpus=4,
                           qemu="qemu-system-x86_64")
                try:
                    st = q64.wait_serial("Procesory:", timeout=90)
                    m = re.search(r"Procesory: (\d+) \(obudzonych (\d+)", st)
                    check("jadro 64-bitowe wylicza wszystkie procesory",
                          m is not None and int(m.group(1)) == 4)
                    check("rdzenie wtorne same dochodza do trybu dlugiego",
                          m is not None and int(m.group(2)) == 3)

                    q64.wait_serial("proton64>", timeout=40)
                    prog = q64.command("run hello64", "kod wyjscia",
                                         timeout=40)
                    check("program w ring 3 dziala jak dotad",
                          "[hello64] koncze sie grzecznie" in prog)

                    work = q64.command("smp", "Liczylo rdzeni",
                                          timeout=90)
                    check("praca rozdzielona na rdzenie takze w trybie "
                          "dlugim", "zgadza sie" in work and
                          "NIE ZGADZA SIE" not in work)
                    m2 = re.search(r"Liczylo rdzeni: (\d+)", work)
                    check("prace liczylo kilka rdzeni naraz (tryb dlugi)",
                          m2 is not None and int(m2.group(1)) >= 2)

                    # Wlasny zegar rdzenia w trybie dlugim.
                    #
                    # Kosztowal osobny blad i to taki, ktorego nie widac
                    # z boku: rdzen wtorny nie wlaczal sobie w EFER bitu
                    # zakazu wykonywania. Jadro mapuje tym bitem rejestry
                    # sprzetu, a bez NXE bit 63 wpisu tablicy stron jest
                    # ZAREZERWOWANY - wiec pierwsze siegniecie po lokalny
                    # APIC konczylo sie bledem strony i cichym zgonem
                    # calej maszyny.
                    clock = values(work, "Zegary rdzeni:")
                    check("kazdy rdzen wtorny ma wlasny zegar takze "
                          "w trybie dlugim (%s)" % clock,
                          len(clock) == 4 and all(z > 0 for z in clock[1:]))
                    time.sleep(1.5)
                    clock2 = values(q64.command("smp", "Zegary rdzeni",
                                                  timeout=90),
                                    "Zegary rdzeni:")
                    check("i tykaja dalej (%s -> %s)" % (clock, clock2),
                          len(clock2) == 4 and
                          all(clock2[i] > clock[i] for i in range(1, 4)))
                finally:
                    q64.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)
            for f in (iso, slog64):
                if os.path.exists(f):
                    os.unlink(f)

    # --- Jeden rdzen: nic sie nie zmienia ---
    print("--- maszyna jednordzeniowa ---")
    slog1 = tempfile.mktemp(suffix=".log")
    q1 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog1)
    try:
        start = q1.wait_for("Procesory:", timeout=60)
        check("na jednym rdzeniu system mowi to wprost i nikogo nie budzi",
              "Procesory: 1" in start)
        q1.wait_for("ProtonOS shell", timeout=30)
        up = q1.command("uptime", "tyknac timera")
        check("i dziala tak samo", "tyknac timera" in up)
    finally:
        q1.close()
        if os.path.exists(slog1):
            os.unlink(slog1)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
