#!/usr/bin/env python3
"""Test dymny ProtonOS: shell, sygnaly i SSE (jadro 64-bitowe).

System, z ktorym da sie rozmawiac, a nie tylko odtworzyc scenariusz:
program uruchamiany Z KLAWIATURY, sygnal zatrzymujacy petle bez konca
od zewnatrz, wlasna procedura obslugi wracajaca dokladnie tam, gdzie
przerwano, i shell uzytkownika laczacy programy potokami.

Na koncu liczby zmiennoprzecinkowe. Na x86-64 SSE jest czescia
konwencji wywolan, wiec kompilator uzywa go bez pytania - a rejestry
XMM sa takim samym stanem zadania, jak reszta, i musza przezyc
przelaczenie kontekstu.

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
