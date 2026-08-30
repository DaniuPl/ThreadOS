#!/usr/bin/env python3
"""Test dymny ProtonOS: procesy w pierscieniu 3 (jadro 64-bitowe).

Selektor kodu zna tylko procesor, a jego dwa najnizsze bity mowia,
w ktorym pierscieniu program NAPRAWDE dzialal - i zeby to powiedziec,
musial wejsc do jadra instrukcja syscall.

Dalej idzie wszystko, co proces dostaje i czego mu nie wolno: program
wczytany z pliku (jadro nie widzialo go w czasie budowy), bit NX
sprawdzony wykonaniem wlasnych danych, odmowa na wskazniki do pamieci
jadra, dwa procesy pod tym samym adresem wirtualnym w osobnych
przestrzeniach, oraz fork z kopiowaniem przy zapisie - z rachunkiem
stron, ktory pokazuje, ze kopiuje sie tylko to, co ktos zapisal.

Uruchamiany osobno albo przez scripts/run_tests.py, ktory puszcza
wszystkie grupy rownolegle.
"""

import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def main():
    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")
    # Dysk za kontrolerem AHCI: to ta droga wymagala doklejenia
    # rejestrow kontrolera do mapowania
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.write(b"\0" * DISK_SIZE)

    try:
        check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        q23 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                   sata=True, qemu="qemu-system-x86_64")
        try:
            # Jadro dochodzi do shella samo; wszystko, co ta grupa
            # sprawdza, dzieje sie dopiero potem
            q23.wait_serial("proton64>", timeout=90)

            # Ring 3: selektor kodu zna tylko procesor, a jego
            # dwa najnizsze bity mowia, w ktorym pierscieniu
            # program NAPRAWDE dzialal. Zeby to powiedziec,
            # musial wejsc do jadra instrukcja syscall.
            q23.wait_serial("Wywolania systemowe przez instrukcje",
                            timeout=20)
            r3 = q23.wait_serial("Program w ring 3:", timeout=25)
            m = re.search(r"Program w ring 3: (.+?), cs=(\w+), "
                          r"pierscien (\d)", r3)
            check("program wszedl do jadra instrukcja syscall",
                  m is not None and m.group(1) == "odezwal sie")
            check("procesor potwierdza pierscien 3"
                  + (f" (cs={m.group(2)})" if m else ""),
                  m is not None and m.group(3) == "3")

            # Program z PLIKU: jadro nie widzialo go w czasie
            # budowy - wszystkiego dowiaduje sie z naglowka ELF
            ini = q23.wait_serial("initrd:", timeout=25)
            check("jadro 64-bitowe czyta initrd od loadera",
                  re.search(r"initrd: \d+ plik", ini) is not None)
            ld = q23.wait_serial("z initrd:", timeout=25)
            m = re.search(r"Program 'hello64' z initrd: (\d+) B, "
                          r"wejscie ([0-9a-f]+)", ld)
            check("ELF64 z initrd zaladowany"
                  + (f" ({m.group(1)} B, wejscie {m.group(2)})"
                     if m else ""),
                  m is not None and int(m.group(1)) > 0
                  # punkt wejscia musi lezec w przestrzeni
                  # uzytkownika, czyli od 1 GiB w gore
                  and int(m.group(2), 16) >= 0x40000000)
            out64 = q23.wait_serial("[hello64] koncze", timeout=30)
            check("program z pliku pisze przez wywolanie systemowe",
                  "[hello64] czesc, jestem ELF-em 64-bitowym"
                  in out64)
            check("program liczy i usypia sam siebie",
                  "[hello64] licze: 1" in out64
                  and "[hello64] licze: 3" in out64)
            m = re.search(r"uplynelo tyknac: (\d+)", out64)
            check("sen programu trwa tyle, ile ma"
                  + (f" ({m.group(1)} tyknac)" if m else ""),
                  m is not None and 13 <= int(m.group(1)) <= 25)
            fin = q23.wait_serial("ELF64 z initrd:", timeout=25)
            m = re.search(r"pierscien (\d), kod wyjscia (\d+)", fin)
            check("program z pliku dzialal w ring 3",
                  m is not None and m.group(1) == "3")
            check("kod wyjscia programu dotarl do jadra"
                  + (f" ({m.group(2)})" if m else ""),
                  m is not None and m.group(2) == "7")

            # Ochrona pamieci widziana od strony programu.
            # Bit NX byl dotad tylko ustawiony - tu program
            # naprawde probuje wykonac swoje dane i ginie.
            nx = q23.wait_serial("Zakaz wykonywania:", timeout=30)
            check("program probuje wykonac wlasne dane",
                  "[nx64] i probuje ja wykonac" in nx)
            check("procesor odmawia wykonania danych (bit NX)",
                  "procesor odmowil wykonania danych" in nx)
            # 0x15 = strona obecna, dostep z ring 3, pobranie
            # instrukcji. Sam bit obecnosci wykluczylby zwykly
            # brak mapowania - to naprawde zadzialal NX.
            m = re.search(r"Zakaz wykonywania: .*err=([0-9a-f]+)", nx)
            check("blad strony mowi: pobranie instrukcji z ring 3"
                  + (f" (err={m.group(1)})" if m else ""),
                  m is not None and int(m.group(1), 16) & 0x15 == 0x15)
            check("winny program zostaje zabity, nie caly system",
                  "zabite: Page Fault" in nx)

            # Druga droga do pamieci jadra: poprosic o nia samo
            # jadro. Musi odmowic, bo sprawdza wskaznik.
            wild = q23.wait_serial("Pamiec jadra:", timeout=30)
            check("jadro odrzuca wskaznik spoza programu",
                  "nie nalezy do programu" in wild
                  and "[wild64] jadro odmowilo" in wild)
            check("pamiec jadra niedostepna z ring 3",
                  "niedostepna dla pierscienia 3" in wild)
            m = re.search(r"Pamiec jadra: .*err=([0-9a-f]+), "
                          r"adres=([0-9a-f]+)", wild)
            check("blad strony mowi: odczyt z ring 3 strony jadra"
                  + (f" (err={m.group(1)}, adres={m.group(2)})"
                     if m else ""),
                  m is not None and int(m.group(1), 16) == 0x05
                  and int(m.group(2), 16) == 0x100000)

            # I to, po co ta cala obsluga istnieje
            surv = q23.wait_serial("System przezyl", timeout=30)
            m = re.search(r"licznik (\d+) -> (\d+)", surv)
            check("system pracuje po smierci obu programow"
                  + (f" (licznik {m.group(1)} -> {m.group(2)})"
                     if m else ""),
                  m is not None
                  and int(m.group(2)) > int(m.group(1)))

            # Dwa procesy naraz, kazdy we wlasnej przestrzeni.
            # Ten sam plik, ten sam adres wirtualny, osobna
            # pamiec - i to wlasnie musi byc widac.
            sep = q23.wait_serial("Pamiec po procesach", timeout=40)
            addrs = re.findall(r"proces (\d): adres ([0-9a-f]+), "
                               r"wpisalem (\d+)", sep)
            check(f"dwa procesy ruszyly naraz ({len(addrs)})",
                  len(addrs) == 2)
            check("oba pisza pod TEN SAM adres wirtualny"
                  + (f" ({addrs[0][1]})" if addrs else ""),
                  len(addrs) == 2 and addrs[0][1] == addrs[1][1])
            check("kazdy wpisal co innego"
                  + (f" ({addrs[0][2]} i {addrs[1][2]})"
                     if len(addrs) == 2 else ""),
                  len(addrs) == 2 and addrs[0][2] != addrs[1][2])
            kept = re.findall(r"proces \d: nadal (\d+) - moja "
                              r"pamiec nietknieta", sep)
            check(f"zaden nie zobaczyl pamieci drugiego "
                  f"({len(kept)}/2)", len(kept) == 2)
            check("obie kopie skonczyly sie bez bledu",
                  "Dwa procesy naraz: 2 zakonczonych, 0 bledow"
                  in sep)
            # Przestrzen musi zniknac razem z procesem, inaczej
            # kazdy uruchomiony program zabiera pamiec na zawsze
            m = re.search(r"Pamiec po procesach oddana "
                          r"\((\d+) -> (\d+) ramek\)", sep)
            check("pamiec po procesach wrocila do puli"
                  + (f" ({m.group(1)} -> {m.group(2)})"
                     if m else ""),
                  m is not None
                  and int(m.group(2)) <= int(m.group(1)))

            # fork: drugi proces bierze sie juz nie z decyzji
            # jadra, tylko z decyzji samego programu
            fk = q23.wait_serial("Pamiec po fork-u", timeout=45)
            m = re.search(r"rodzic: fork dal numer dziecka (\d+)", fk)
            check("rodzic dostaje z fork-a numer dziecka"
                  + (f" ({m.group(1)})" if m else ""),
                  m is not None and int(m.group(1)) > 0)
            check("dziecko dostaje z fork-a zero",
                  "dziecko: fork dal 0" in fk)
            # Oba wracaja w to samo miejsce z tym samym stanem,
            # ale pamiec maja juz osobna
            check("dziecko pisze po swojej kopii (200)",
                  "moja wartosc to 200" in fk)
            check("zapis dziecka nie dosiega rodzica (nadal 100)",
                  "rodzic: nadal 100 - dziecko dostalo kopie" in fk)
            check("obie galezie skonczyly sie bez bledu",
                  "Proces powielil sie sam: 2 zakonczonych, "
                  "0 bledow" in fk)
            m = re.search(r"Pamiec po fork-u oddana "
                          r"\((\d+) -> (\d+) ramek\)", fk)
            check("kopia pamieci wrocila do puli"
                  + (f" ({m.group(1)} -> {m.group(2)})"
                     if m else ""),
                  m is not None
                  and int(m.group(2)) <= int(m.group(1)))

            # fork + exec + wait: klasyczna trojka, ktora
            # wystarcza kazdemu shellowi na swiecie
            sp = q23.wait_serial("fork + exec + wait:", timeout=60)
            check("proces uruchamia INNY program przez exec",
                  "powielam sie i uruchamiam w dziecku 'hello64'"
                  in sp
                  and "[hello64] czesc, jestem ELF-em" in sp)
            m = re.search(r"doczekalem dziecka (\d+), "
                          r"kod wyjscia (\d+)", sp)
            check("rodzic doczekal wlasnego dziecka"
                  + (f" (nr {m.group(1)})" if m else ""),
                  m is not None and int(m.group(1)) > 0)
            # hello64 konczy sie siodemka - ta siodemka jest
            # dowodem, ze doczekal sie TEGO dziecka, a nie
            # dowolnego zakonczonego zadania
            check("kod wyjscia doszedl do rodzica przez wait"
                  + (f" ({m.group(2)})" if m else ""),
                  m is not None and m.group(2) == "7")
            check("wait bez dzieci mowi, ze nie ma na co czekac",
                  "BLAD: wait wymyslil sobie dziecko" not in sp)
            check("program sam potwierdza cala trojke",
                  "[spawn64] fork + exec + wait dzialaja" in sp)
            check("jadro potwierdza werdykt programu",
                  "fork + exec + wait: 2 zakonczonych, werdykt 0"
                  in sp)

            # A teraz rachunek: fork NIE kopiuje pamieci.
            # Program ma siedem stron (kod, dane stale, dane,
            # cztery strony stosu), wiec kopiowanie wprost
            # kosztowaloby ich siedem plus tablice. Z dzieleniem
            # stron zostaja same tablice, a kopiuje sie tylko to,
            # co ktos naprawde zapisal.
            cow = q23.wait_serial("Kopiowanie przy zapisie",
                                  timeout=30)
            m = re.search(r"fork wzial (\d+) ramek, "
                          r"rozdzielono (\d+) stron", cow)
            check("fork nie kopiuje pamieci programu"
                  + (f" ({m.group(1)} ramek - same tablice)"
                     if m else ""),
                  m is not None and int(m.group(1)) <= 6)
            check("rozdzielone tylko strony naprawde zapisane"
                  + (f" ({m.group(2)})" if m else ""),
                  m is not None and 1 <= int(m.group(2)) <= 6)
        finally:
            q23.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, slog2, disk):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
