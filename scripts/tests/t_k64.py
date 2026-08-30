#!/usr/bin/env python3
"""Test dymny ProtonOS: jadro 64-bitowe w trybie dlugim.

Uruchamiany osobno albo przez scripts/run_testy.py, ktory puszcza
wszystkie grupy rownolegle.
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
from ptest import check, summary


def main():
    # znacznikow - to te same pliki, co w jadrze 32-bitowym.
    print("--- jadro 64-bitowe (tryb dlugi) ---")
    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("POMINIETE: brak obrazu 64-bitowego (make kernel64)")
    elif shutil.which("grub-mkrescue") is None:
        print("POMINIETE: brak grub-mkrescue")
    elif shutil.which("qemu-system-x86_64") is None:
        print("POMINIETE: brak qemu-system-x86_64 (tryb dlugi)")
    else:
        d64 = tempfile.mkdtemp()
        iso64 = tempfile.mktemp(suffix=".iso")
        slog64 = tempfile.mktemp(suffix=".log")
        slog64b = tempfile.mktemp(suffix=".log")
        # Dysk za kontrolerem AHCI: to ta droga wymagala doklejenia
        # rejestrow kontrolera do mapowania, wiec ja wlasnie sprawdzamy
        disk64 = tempfile.mktemp(suffix=".img")
        with open(disk64, "wb") as fh:
            fh.write(b"\0" * DISK_SIZE)
        try:
            os.makedirs(d64 + "/boot/grub")
            shutil.copy(KERNEL64, d64 + "/boot/protonos64.bin")
            mod = ""
            if INITRD64 and os.path.exists(INITRD64):
                shutil.copy(INITRD64, d64 + "/boot/initrd64.img")
                mod = "    module2 /boot/initrd64.img initrd64\n"
            with open(d64 + "/boot/grub/grub.cfg", "w") as fh:
                fh.write("set timeout=0\nset default=0\n"
                         'menuentry "ProtonOS 64" {\n'
                         "    multiboot2 /boot/protonos64.bin\n"
                         + mod + "}\n")
            made = subprocess.run(["grub-mkrescue", "-o", iso64, d64],
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL).returncode == 0
            check("obraz ISO z jadrem 64-bitowym buduje sie", made)
            if made:
                q23 = Qemu(KERNEL, INITRD, cdrom=iso64, serial=slog64,
                           disk=disk64, sata=True,
                           qemu="qemu-system-x86_64")
                try:
                    out = q23.wait_serial("Tryb dlugi:", timeout=40)
                    check("jadro startuje w trybie dlugim",
                          "Jadro uruchomione w trybie dlugim" in out)
                    # LMA ustawia sam procesor - to jego zdanie, nie nasze
                    check("procesor potwierdza tryb dlugi (LMA=1)",
                          "LMA=1" in out)
                    check("wskazniki maja osiem bajtow",
                          "wskaznik ma 8 bajtow" in out)
                    check("mapa pamieci czytana tym samym kodem",
                          re.search(r"Pamiec fizyczna: \d{5,} KiB RAM",
                                    q23.wait_serial("Pamiec fizyczna",
                                                    timeout=20)) is not None)
                    check("bufor obrazu od loadera rozpoznany",
                          re.search(r"Bufor obrazu: \d+x\d+",
                                    q23.wait_serial("Bufor obrazu",
                                                    timeout=20)) is not None)

                    # Pamiec: wlasne tablice zbudowane z ramek od PMM,
                    # a nie te ulozone na sztywno w obrazie jadra
                    mem = q23.wait_serial("Stronicowanie czteropoziomowe",
                                          timeout=20)
                    m = re.search(r"czteropoziomowe: (\d+) MiB, zakaz "
                                  r"wykonywania (\w+)", mem)
                    check("wlasne tablice stron obejmuja cala pamiec"
                          + (f" ({m.group(1)} MiB)" if m else ""),
                          m is not None and int(m.group(1)) >= 100)
                    check("zakaz wykonywania wlaczony (bit NXE)",
                          m is not None and m.group(2) == "wlaczony")
                    # Strona wstawiona osiem gigabajtow ponad obszar
                    # opisany identycznosciowo - zapis i odczyt przez
                    # nia przechodza cala czterostopniowa sciezke
                    hi = q23.wait_serial("Mapowanie 4 KiB", timeout=20)
                    check("mapowanie 4 KiB daleko poza pamiecia dziala",
                          "odczyt 123456789abcdef" in hi)
                    heap = q23.wait_serial("Sterta jadra", timeout=20)
                    check("sterta jadra dziala w trybie dlugim",
                          "kmalloc/kfree dzialaja" in heap
                          and "po zwolnieniu 0 B" in heap)

                    # Przerwania w trybie dlugim: pulapka int3 przechodzi
                    # cala droge - stub, tablice, ramke rejestrow, iretq -
                    # i wraca, nie zatrzymujac systemu
                    trap = q23.wait_serial("[INT3]", timeout=20)
                    check("wyjatek-pulapka obsluzony i wznowiony",
                          re.search(r"\[INT3\] breakpoint pod rip=",
                                    trap) is not None)
                    # A timer musi tykac sam z siebie - to dowod, ze
                    # przerwania sprzetowe dochodza przez nowa tablice

                    # Watki: liczniki musza zgadzac sie z dlugoscia snu
                    # co do jednego. Przez 600 ms watek spiacy po 20 ms
                    # zdazy 30 razy, a ten po 50 ms - 12. Zepsute
                    # przelaczanie kontekstu albo usypianie od razu
                    # wypycha te liczby poza zakres.
                    sch = q23.wait_serial("Watki licza rownolegle",
                                          timeout=25)
                    m = re.search(r"A=(\d+), B=(\d+)", sch)
                    check("watki dzialaja rownolegle, a sen trwa tyle, ile ma"
                          + (f" (A={m.group(1)}, B={m.group(2)})"
                             if m else ""),
                          m is not None and 28 <= int(m.group(1)) <= 32
                          and 11 <= int(m.group(2)) <= 13)
                    zombie = q23.wait_serial("Watek konczacy sie", timeout=20)
                    check("zakonczony watek znika z listy",
                          "zrobil swoje" in zombie
                          and "zadan na liscie: 4" in zombie)

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

                    # Klawiatura i shell: system, z ktorym da sie
                    # rozmawiac, a nie tylko odtworzyc scenariusz
                    ready = q23.wait_serial("proton64>", timeout=40)
                    check("jadro 64-bitowe uruchamia shella",
                          "Klawiatura PS/2 (IRQ 1)" in ready
                          and "System 64-bitowy gotowy" in ready)

                    q23.type_line("mem")
                    mem = q23.wait_serial("Pamiec:", timeout=20)
                    check("shell odpowiada na 'mem'",
                          re.search(r"Pamiec: \d+ KiB RAM, ramki "
                                    r"\d+/\d+ zajete", mem) is not None)
                    q23.type_line("ls")
                    lsout = q23.wait_serial("hello64  (", timeout=20)
                    check("shell wypisuje programy z initrd",
                          "spawn64" in lsout and "sep64" in lsout)
                    q23.type_line("ps")
                    ps = q23.wait_serial("shell (biezacy)", timeout=20)
                    check("shell widzi sam siebie na liscie zadan",
                          "bezczynny" in ps)

                    # Program uruchomiony Z KLAWIATURY, nie ze scenariusza
                    q23.type_line("run hello64")
                    run = q23.wait_serial("koniec, kod wyjscia", timeout=40)
                    check("program uruchomiony z klawiatury dziala",
                          "[hello64] czesc, jestem ELF-em" in run)
                    check("shell doczekal programu i podal kod wyjscia",
                          "[hello64] koniec, kod wyjscia 7" in run)
                    q23.type_line("nieznane")
                    bad = q23.wait_serial("Nieznane polecenie", timeout=20)
                    check("shell nie udaje, ze zna kazde polecenie",
                          "Nieznane polecenie: nieznane" in bad)

                    # Sygnaly: program w petli bez konca musi dac sie
                    # zatrzymac z zewnatrz, bo od srodka sie nie da
                    q23.type_line("run spin64")
                    q23.wait_serial("[spin64] krece sie", timeout=30)
                    time.sleep(1)
                    q23.sendkey("ctrl-c")
                    kill = q23.wait_serial("[spin64] koniec", timeout=30)
                    check("Ctrl+C wysyla sygnal do programu",
                          re.search(r"zadanie \d+ dostalo sygnal 2", kill)
                          is not None)
                    check("program bez wlasnej procedury ginie od sygnalu",
                          "(spin64) zabite: sygnal" in kill)
                    check("shell odzyskuje konsole po zabitym programie",
                          "[spin64] koniec, kod wyjscia -1" in kill)

                    # A program z wlasna procedura ma po niej wrocic
                    # tam, gdzie mu przerwano - to jest cala roznica
                    # miedzy sygnalem a zabiciem
                    q23.type_line("run sigtest64")
                    q23.wait_serial("[sigtest64] czekam", timeout=30)
                    time.sleep(1)
                    q23.sendkey("ctrl-c")
                    sig = q23.wait_serial("[sigtest64] koniec", timeout=30)
                    check("program przechwytuje sygnal wlasna procedura",
                          "procedura obslugi: dostalem sygnal" in sig)
                    check("po procedurze program wraca w przerwane miejsce",
                          "wrocilem do przerwanego miejsca" in sig)
                    check("przechwycony sygnal nie zabija programu",
                          "[sigtest64] koniec, kod wyjscia 0" in sig)
                    check("jadro nie zglasza smierci przechwyconego sygnalu",
                          "(sigtest64) zabite" not in sig)

                    # Tylko to, co doszlo PO tym poleceniu - wczesniej
                    # w logu oba programy oczywiscie sa
                    seen = len(q23.serial())
                    q23.type_line("ps")
                    after = q23.wait_serial("shell (biezacy)",
                                            timeout=20)[seen:]
                    check("po obu programach nie zostaje zadne zadanie",
                          "spin64" not in after
                          and "sigtest64" not in after
                          and "bezczynny" in after)

                    # Shell w PRZESTRZENI UZYTKOWNIKA: zwykly proces
                    # bez zadnych uprawnien ponad te, ktore ma kazdy
                    # inny - czyta klawiature wywolaniem systemowym,
                    # a programy uruchamia przez fork i exec
                    q23.type_line("run ush64")
                    u = q23.wait_serial("ush64$", timeout=40)
                    check("shell uzytkownika startuje jako proces ring 3",
                          "ush64 - shell uzytkownika" in u)

                    # Slowo musi pojawic sie DWA razy: raz jako echo
                    # pisania, raz jako odpowiedz programu. Liczymy
                    # wystapienia zamiast szukac konca linii - port
                    # szeregowy konczy je sekwencja "\n\r", a wiazanie
                    # sie z takim szczegolem to proszenie sie o klopoty.
                    q23.type_line("echo abrakadabra")
                    limit = time.time() + 25
                    while (time.time() < limit
                           and q23.serial().count("abrakadabra") < 2):
                        time.sleep(0.3)
                    ec = q23.serial()
                    check("program czyta klawiature wywolaniem systemowym"
                          f" ({ec.count('abrakadabra')} wystapien)",
                          ec.count("abrakadabra") >= 2)

                    seen = len(q23.serial())
                    q23.type_line("uecho64 z shella uzytkownika")
                    ua = q23.wait_serial("ush64: koniec, kod 0",
                                         timeout=40)[seen:]
                    check("shell uzytkownika przekazuje argumenty do exec",
                          "z shella uzytkownika" in ua)

                    # Potoki: wyjscie jednego programu wchodzi na
                    # wejscie drugiego, a zaden z nich o tym nie wie.
                    # uupper64 czyta z numeru zero i pisze na numer
                    # jeden - to, co po drugiej stronie, jest sprawa
                    # tego, kto go uruchomil.
                    seen = len(q23.serial())
                    q23.type_line("uecho64 proton przez potok | uupper64")
                    pp = q23.wait_serial("PROTON PRZEZ POTOK",
                                         timeout=40)[seen:]
                    check("wyjscie programu wchodzi na wejscie drugiego",
                          "PROTON PRZEZ POTOK" in pp)

                    # Drugi program musi zobaczyc KONIEC danych - a to
                    # sie dzieje tylko wtedy, gdy shell zamknie oba
                    # konce potoku u siebie
                    seen = len(q23.serial())
                    q23.type_line("uls64 | uupper64")
                    pl = q23.wait_serial("ULS64: WPISOW", timeout=40)[seen:]
                    check("czytajacy widzi koniec danych i konczy sie sam",
                          "ULS64: WPISOW" in pl)

                    seen = len(q23.serial())
                    q23.type_line("uecho64 sam |")
                    pe = q23.wait_serial("po kresce nic nie ma",
                                         timeout=30)[seen:]
                    check("pusta strona potoku nie wywraca shella",
                          "ush64: po kresce nic nie ma" in pe)

                    q23.type_line("hello64")
                    hi = q23.wait_serial("ush64: koniec, kod 7", timeout=45)
                    check("shell uzytkownika uruchamia program (fork+exec)",
                          "[hello64] czesc, jestem ELF-em" in hi)
                    check("i odbiera jego kod wyjscia przez wait",
                          "ush64: koniec, kod 7" in hi)

                    # Nieudany exec nie moze zabic shella - dziecko
                    # konczy sie samo, rodzic zyje dalej
                    q23.type_line("nieistniejacy")
                    ne = q23.wait_serial("ush64: koniec, kod 127",
                                         timeout=30)
                    check("nieudany exec konczy tylko dziecko",
                          "nie ma programu 'nieistniejacy'" in ne
                          and "ush64: koniec, kod 127" in ne)

                    # Ctrl+C w shellu uzytkownika. Sygnal musi trafic
                    # w PROGRAM, ktory dziala, a nie w shella, ktory na
                    # niego czeka - i musi dosiegnac shella czekajacego
                    # w jadrze, bo inaczej czeka on tam bez konca.
                    q23.type_line("spin64")
                    q23.wait_serial("[spin64] krece sie", timeout=30)
                    time.sleep(1.5)
                    q23.sendkey("ctrl-c")
                    ci = q23.wait_serial("ush64: koniec, kod -1", timeout=30)
                    check("Ctrl+C trafia w program, nie w czekajacy shell",
                          "(spin64) zabite: sygnal" in ci)
                    check("kod wyjscia zabitego programu jest ujemny",
                          "ush64: koniec, kod -1" in ci)
                    # A sam shell ma to przezyc - inaczej kazde
                    # przypadkowe Ctrl+C konczyloby sesje
                    seen = len(q23.serial())
                    q23.type_line("echo poctrlc")
                    limit = time.time() + 25
                    while (time.time() < limit
                           and q23.serial()[seen:].count("poctrlc") < 2):
                        time.sleep(0.3)
                    check("shell uzytkownika przezyl Ctrl+C i czyta dalej",
                          q23.serial()[seen:].count("poctrlc") >= 2)

                    q23.type_line("exit")
                    ex = q23.wait_serial("[ush64] koniec, kod wyjscia 0",
                                         timeout=30)
                    check("po wyjsciu shella uzytkownika wraca shell jadra",
                          "ush64: pa!" in ex)

                    # Liczby zmiennoprzecinkowe. Na x86-64 SSE jest
                    # czescia konwencji wywolan, wiec kompilator uzywa
                    # go bez pytania - a rejestry XMM sa takim samym
                    # stanem zadania, jak reszta.
                    sse = q23.wait_serial("Stan SSE przetrwal", timeout=40)
                    check("jadro wlacza jednostke zmiennoprzecinkowa",
                          "Jednostka zmiennoprzecinkowa i SSE wlaczone"
                          in sse)
                    results = re.findall(r"\[fpu64\] start (\d+): wyszlo "
                                        r"(\d+), mialo (\d+) - (\S+)", sse)
                    check(f"dwa procesy licza rownoczesnie "
                          f"({len(results)})", len(results) == 2)
                    # Kazdy ma dostac SWOJ wynik - gdyby stan SSE nie
                    # byl zapisywany, dostalby wynik drugiego
                    check("kazdy dostaje wlasny wynik, nie cudzy",
                          len(results) == 2
                          and results[0][1] != results[1][1]
                          and all(w[1] == w[2] for w in results))
                    check("jadro potwierdza brak pomylek",
                          "2 policzylo, 0 pomylek" in sse)

                    # Dysk: te same sterowniki, co w jadrze 32-bitowym,
                    # skompilowane drugi raz. Rejestry kontrolera leza
                    # wysoko, poza pamiecia opisana przy starcie - bez
                    # doklejenia ich do mapowania pierwszy odczyt z BAR-a
                    # konczylby sie bledem strony.
                    # Jedna linia mowi o obu warstwach: co znalazl
                    # sterownik i czy stanal na tym system plikow
                    dsk = q23.wait_serial("] Dysk ", timeout=30)
                    m = re.search(r"\] Dysk (.+?) \+ PFS (\w+) "
                                  r"\((\d+) sektorow\)", dsk)
                    check("jadro 64-bitowe znajduje dysk przez PCI i AHCI"
                          + (f" ({m.group(1)}, {m.group(3)} sektorow)"
                             if m else ""),
                          m is not None and "AHCI" in m.group(1)
                          and int(m.group(3)) > 0)
                    check("PFS staje na dysku widzianym przez AHCI",
                          m is not None and m.group(2) == "gotowy")

                    q23.type_line("disk")
                    io = q23.wait_serial("disk: sektor", timeout=30)
                    m = re.search(r"disk: sektor (\d+) zapisany i "
                                  r"odczytany, roznic (\d+)", io)
                    check("zapis i odczyt sektora przez DMA oddaja to samo"
                          + (f" (sektor {m.group(1)})" if m else ""),
                          m is not None and m.group(2) == "0")

                    # Sterta programu. Prosba o pamiec nic nie
                    # kosztuje - placi sie za strony naprawde dotkniete,
                    # bo powstaja dopiero pod palcami.
                    #
                    # Ten program jest przy okazji sprawdzianem SSE
                    # w kodzie uzytkownika: kompilator wektoryzuje jego
                    # petle przepisujace 4 KiB i wstawia tam movdqa,
                    # ktore bez wlaczonego SSE konczy sie wyjatkiem.
                    seen = len(q23.serial())
                    q23.type_line("run heap64")
                    hp = q23.wait_serial("[heap64] koniec, kod wyjscia",
                                         timeout=45)[seen:]
                    m = re.search(r"prosze o (\d+) KiB \((\d+) stron\)", hp)
                    check("program prosi jadro o pamiec w biegu"
                          + (f" ({m.group(1)} KiB)" if m else ""),
                          m is not None and int(m.group(2)) >= 1000)
                    check("pamiec oddaje to, co program w nia wpisal",
                          "wszystkie oddaly to, co wpisalem" in hp)
                    check("granica sterty wraca po oddaniu pamieci",
                          "granica wrocila na miejsce" in hp)
                    check("ponizej wlasnej sterty jadro nie puszcza",
                          "ponizej wlasnej sterty jadro nie puszcza" in hp)
                    # I rachunek: 1024 strony poproszone, cztery uzyte
                    m2 = re.search(r"ramek (\d+) -> (\d+), "
                                   r"stron sterty (\d+)", hp)
                    check("strony powstaja dopiero przy dotknieciu"
                          + (f" (z {m.group(2) if m else '?'} "
                             f"poproszonych: {m2.group(3)})" if m2 else ""),
                          m2 is not None and 1 <= int(m2.group(3)) <= 8)
                    check("po programie nie zostaje ani jedna ramka"
                          + (f" ({m2.group(1)} -> {m2.group(2)})"
                             if m2 else ""),
                          m2 is not None
                          and int(m2.group(2)) <= int(m2.group(1)))

                    # Argumenty programu. Do tej pory 'echo' musialo
                    # byc wbudowane w shell, bo nie bylo jak przekazac
                    # programowi ani jednego slowa.
                    seen = len(q23.serial())
                    q23.type_line("run uecho64 raz dwa trzy")
                    ue = q23.wait_serial("[uecho64] koniec, kod wyjscia",
                                         timeout=40)[seen:]
                    check("program dostaje argumenty z linii polecen",
                          "raz dwa trzy" in ue)
                    check("i konczy sie bez bledu",
                          "[uecho64] koniec, kod wyjscia 0" in ue)

                    # Narzedzie, ktore do niedawna musialo siedziec
                    # w jadrze: bierze nazwe pliku z argumentu, otwiera
                    # go deskryptorem i czyta
                    q23.type_line("write zargv tresc dla ucat")
                    q23.wait_serial("write: 'zargv' zapisany", timeout=30)
                    seen = len(q23.serial())
                    q23.type_line("run ucat64 zargv")
                    uc = q23.wait_serial("[ucat64] koniec, kod wyjscia",
                                         timeout=40)[seen:]
                    check("program czyta plik podany w argumencie",
                          "tresc dla ucat" in uc
                          and "[ucat64] koniec, kod wyjscia 0" in uc)

                    seen = len(q23.serial())
                    q23.type_line("run ucat64")
                    ub = q23.wait_serial("[ucat64] koniec, kod wyjscia",
                                         timeout=40)[seen:]
                    check("bez argumentu program mowi, czego mu brakuje",
                          "ucat64: podaj nazwe pliku" in ub
                          and "kod wyjscia 1" in ub)

                    # Siec: karta, warstwa IP, DHCP i TCP to znowu te
                    # same pliki, co w jadrze 32-bitowym. Trzy z czterech
                    # przeszly bez jednej zmiany - sterownik karty
                    # potrzebowal tylko jawnego rzutowania adresow, bo
                    # karta adresuje pamiec 32 bitami.
                    kar = q23.wait_serial("] Siec: RTL8139", timeout=40)
                    check("jadro 64-bitowe znajduje karte sieciowa",
                          re.search(r"Siec: RTL8139 ([0-9a-f]+:){5}",
                                    kar) is not None)
                    dh = q23.wait_serial("[siec] DHCP", timeout=40)
                    m = re.search(r"DHCP: przyznano (\d+\.\d+\.\d+\.\d+), "
                                  r"brama (\d+\.\d+\.\d+\.\d+)", dh)
                    check("adres IP przychodzi z sieci przez DHCP"
                          + (f" ({m.group(1)}, brama {m.group(2)})"
                             if m else ""),
                          m is not None)

                    seen = len(q23.serial())
                    q23.type_line("ifconfig")
                    ifc = q23.wait_serial("RX ", timeout=25)[seen:]
                    check("ifconfig pokazuje adres, maske, brame i DNS",
                          re.search(r"IP \d+\.\d+\.\d+\.\d+  maska ", ifc)
                          is not None)

                    # Odpowiedz bramy to jedyny dowod, ze ramki naprawde
                    # wychodza i wracaja - reszta to tylko konfiguracja
                    seen = len(q23.serial())
                    q23.type_line("ping 10.0.2.2")
                    pg = q23.wait_serial("--- statystyki", timeout=40)[seen:]
                    m = re.search(r"wyslano (\d+), odebrano (\d+)", pg)
                    check("brama odpowiada na ping"
                          + (f" ({m.group(2)}/{m.group(1)})" if m else ""),
                          m is not None and m.group(1) == m.group(2)
                          and int(m.group(2)) > 0)

                    seen = len(q23.serial())
                    q23.type_line("arp")
                    ar = q23.wait_serial("10.0.2.2  ", timeout=25)[seen:]
                    check("tablica ARP zna adres sprzetowy bramy",
                          re.search(r"10\.0\.2\.2  ([0-9a-f]+:){5}", ar)
                          is not None)

                    # Katalogi. System plikow przestal byc plaski,
                    # a katalog roboczy jest wlasnoscia zadania - wiec
                    # program widzi ten, w ktorym stal shell.
                    q23.type_line("mkdir projekt")
                    mk = q23.wait_serial("mkdir: 'projekt' zalozony",
                                         timeout=25)
                    check("shell zaklada katalog",
                          "mkdir: 'projekt' zalozony" in mk)
                    seen = len(q23.serial())
                    q23.type_line("cd projekt")
                    q23.type_line("pwd")
                    pw = q23.wait_serial("/projekt", timeout=25)[seen:]
                    check("wejscie do katalogu zmienia sciezke robocza",
                          "/projekt" in pw)

                    q23.type_line("write plan katalogi dzialaja")
                    q23.wait_serial("write: 'plan' zapisany", timeout=25)
                    seen = len(q23.serial())
                    q23.type_line("run uls64")
                    ul = q23.wait_serial("uls64: wpisow", timeout=40)[seen:]
                    check("program dziedziczy katalog roboczy po shellu",
                          "uls64: /projekt" in ul)
                    check("i widzi w nim tylko wlasne wpisy",
                          "plan  (18 B)" in ul and "uls64: wpisow 1" in ul)

                    seen = len(q23.serial())
                    q23.type_line("cd ..")
                    q23.type_line("run uls64")
                    ul2 = q23.wait_serial("uls64: wpisow", timeout=40)[seen:]
                    check("wyjscie w gore wraca do katalogu glownego",
                          "uls64: /" in ul2)
                    check("katalog jest widoczny jako katalog, nie plik",
                          "projekt/  (katalog)" in ul2)

                    q23.type_line("rm nieistnieje")
                    rm = q23.wait_serial("rm: nie ma", timeout=25)
                    check("usuwanie nieistniejacego pliku nie udaje sukcesu",
                          "rm: nie ma 'nieistnieje'" in rm)

                    # Plik od strony PROGRAMU: deskryptor jest zwyklym
                    # numerem w jego wlasnej tablicy, a i-wezel, na
                    # ktory numer wskazuje, zna juz tylko jadro
                    q23.type_line("run fileio64")
                    fio = q23.wait_serial("[fileio64] koniec, kod wyjscia",
                                          timeout=45)
                    check("program w ring 3 zaklada plik i pisze do niego",
                          "zakladam plik i pisze do niego z ring 3" in fio)
                    m = re.search(r"zgadza sie co do bajtu \((\d+) B\)", fio)
                    check("odczyt z pliku oddaje to, co program zapisal"
                          + (f" ({m.group(1)} B)" if m else ""),
                          m is not None and int(m.group(1)) > 0)
                    check("jadro nie otwiera pliku, ktorego nie ma",
                          "nieistniejacego pliku jadro nie otwiera" in fio)
                    check("program sam potwierdza wynik kodem wyjscia",
                          "[fileio64] koniec, kod wyjscia 0" in fio)

                    # fs.c przeszedl do trybu dlugiego bez zmiany ani
                    # jednego znaku - dysk widziany przez warstwe blokowa
                    # wyglada tak samo w obu trybach
                    q23.type_line("write notatka pisze na dysk")
                    wr = q23.wait_serial("write: 'notatka' zapisany",
                                         timeout=30)
                    check("shell zapisuje plik na dysku",
                          re.search(r"write: 'notatka' zapisany \(\d+ B\)",
                                    wr) is not None)
                    q23.type_line("fls")
                    fl = q23.wait_serial("Zajetosc:", timeout=25)
                    check("plik widac na liscie katalogu glownego",
                          re.search(r"notatka  \(\d+ B\)", fl) is not None)
                    seen = len(q23.serial())
                    q23.type_line("cat notatka")
                    ct = q23.wait_serial("pisze na dysk", timeout=25)[seen:]
                    check("odczyt pliku oddaje to, co zapisano",
                          "pisze na dysk" in ct)

                    ticks = q23.wait_serial("Timer tyka:", timeout=25)
                    m = re.search(r"Timer tyka: (\d+) -> (\d+)", ticks)
                    check("przerwania sprzetowe dochodza (timer tyka"
                          + (f" {m.group(1)} -> {m.group(2)})" if m else ")"),
                          m is not None and int(m.group(2)) - int(m.group(1)) >= 100)
                    # I to samo na ekranie: konsola rysowana wlasnym kodem
                    w, h, at = q23.screen_raw()
                    x0, y0 = (w - 640) // 2, (h - 200) // 2
                    lit = sum(1 for y in range(y0, y0 + 200)
                              for x in range(x0, x0 + 640)
                              if at(x, y) != (0, 0, 0))
                    check(f"konsola 64-bitowa rysuje na buforze obrazu "
                          f"({lit} pikseli)", lit > 2000)
                finally:
                    q23.close()

                # Drugi rozruch na TYM SAMYM dysku. To jedyny dowod, ze
                # plik naprawde lezy na dysku, a nie w pamieci - i
                # pierwszy moment, w ktorym cokolwiek w tym jadrze
                # przezywa wylaczenie maszyny.
                q23b = Qemu(KERNEL, INITRD, cdrom=iso64, serial=slog64b,
                            disk=disk64, sata=True,
                            qemu="qemu-system-x86_64")
                try:
                    again = q23b.wait_serial("+ PFS", timeout=60)
                    check("drugi rozruch zastaje gotowy system plikow",
                          "+ PFS gotowy" in again
                          and "sformatowany" not in again)
                    q23b.wait_serial("proton64>", timeout=40)
                    q23b.type_line("fls")
                    fl2 = q23b.wait_serial("Zajetosc:", timeout=25)
                    check("plik przezyl wylaczenie maszyny",
                          re.search(r"notatka  \(\d+ B\)", fl2) is not None)
                    seen = len(q23b.serial())
                    q23b.type_line("cat notatka")
                    ct2 = q23b.wait_serial("pisze na dysk",
                                           timeout=25)[seen:]
                    check("i ma dokladnie te sama tresc",
                          "pisze na dysk" in ct2)
                    # Plik zapisany przez PROGRAM tez ma przezyc -
                    # droga przez deskryptor konczy sie na tym samym
                    # dysku, co droga przez jadro
                    seen = len(q23b.serial())
                    q23b.type_line("cat zring3")
                    ct3 = q23b.wait_serial("plik zapisany z ring 3",
                                           timeout=25)[seen:]
                    check("plik zapisany przez program tez przezyl restart",
                          "ProtonOS 64: plik zapisany z ring 3" in ct3)
                finally:
                    q23b.close()
        finally:
            shutil.rmtree(d64, ignore_errors=True)
            for f in (iso64, slog64, slog64b, disk64):
                if os.path.exists(f):
                    os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
