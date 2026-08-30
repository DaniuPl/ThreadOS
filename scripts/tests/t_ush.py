#!/usr/bin/env python3
"""Test dymny ProtonOS: ush - shell uzytkownika w pierscieniu 3.

Shell jadra jest wygoda; ush jest dowodem. Zwykly proces przejmuje
konsole przez sys_read, uruchamia inne programy przez spawn i wait,
laczy je potokami i przekierowuje ich wyjscie do plikow - wszystko
wywolaniami systemowymi, bez zadnej pomocy z zewnatrz.

Liczby w asercjach nie sa ozdoba. Wynik mierzymy przez 'wc', bo sama
tresc na ekranie myli sie z echem wpisanej komendy: "wierszy=3
bajtow=8" jest jednoznaczne, a widok '2' na ekranie nie jest.

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

    # Liczniki watkow demonstracyjnych - do sprawdzania, czy system
    # zyje dalej po tym, jak cos w nim zginelo
    def counters(s):
        a = re.findall(r"watek-A: (\d+)", s)
        b = re.findall(r"watek-B: (\d+)", s)
        return (int(a[-1]) if a else None, int(b[-1]) if b else None)

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        q.wait_for("ProtonOS shell", timeout=40)
        q.wait_for("[siec] DHCP", timeout=30)
        time.sleep(0.5)

        # Shell uzytkownika: sys_read przejmuje konsole
        q.type_line("run sh")
        q.wait_for("ush - shell uzytkownika")
        view = q.wait_for("ush:/")  # prompt pokazuje katalog roboczy (root = /)
        check("ush wystartowal w ring 3", "ush:/" in view)

        q.type_line("pid")
        check("ush zna swoj pid (sys_getpid)",
              "moj pid to" in q.wait_for("moj pid to"))

        q.type_line("hello")
        view = q.wait_for("[hello] koncze sie grzecznie")
        check("ush odpalil hello przez spawn+wait",
              "[hello] koncze sie grzecznie" in view)

        q.type_line("nieistnieje")
        check("ush zglasza nieznany program",
              "ush: nie znam 'nieistnieje'" in q.wait_for("ush: nie znam"))

        # Przekierowanie wyjscia: 'echo ... > plik' (dziedziczenie fd 1)
        q.type_line("echo przekierowanie-ok > redir.txt")
        screen = q.wait_for("wyjscie zapisane do 'redir.txt'")
        # echo nie powinno wypisac tekstu na konsole - poszedl do pliku
        idx = screen.rfind("redir.txt")
        check("echo z '>' nie pisze na konsole (wyjscie do pliku)",
              "przekierowanie-ok" not in screen.split("echo przekierowanie-ok")[-1]
              .split("wyjscie zapisane")[0])
        # ...a 'cat' tego pliku pokazuje zapisana tresc
        q.type_line("cat redir.txt")
        check("przekierowane wyjscie trafilo do pliku (cat redir.txt)",
              "przekierowanie-ok" in q.wait_for("przekierowanie-ok"))

        # Potok: 'echo proton-pipe | wc' - dwa procesy rownolegle,
        # echo pisze do potoku, wc czyta do EOF i liczy ('proton-pipe\n'
        # = 11 znakow + nowa linia = 12 bajtow, 1 wiersz)
        q.type_line("echo proton-pipe | wc")
        screen = q.wait_for("wc: wierszy=")
        check("potok laczy dwa procesy (echo | wc): poprawny licznik",
              "wc: wierszy=1 bajtow=12" in screen)

        # seq jako producent wielu wierszy: 'seq 5 | wc' -> 5 wierszy,
        # 10 bajtow (1\n2\n3\n4\n5\n)
        q.type_line("seq 5 | wc")
        check("seq przez potok do wc: 5 wierszy, 10 bajtow",
              "wc: wierszy=5 bajtow=10" in q.wait_for("wc: wierszy=5"))

        # grep filtruje w potoku; wynik mierzymy przez plik + wc, bo
        # samo '7' na ekranie myli sie z echem wpisanej komendy.
        # seq 9 | grep 7 -> tylko '7' (1 wiersz, 2 bajty)
        q.type_line("seq 9 | grep 7 > g.txt")
        time.sleep(0.5)
        q.type_line("cat g.txt | wc")
        check("grep w potoku przepuszcza tylko pasujace wiersze",
              "wc: wierszy=1 bajtow=2" in q.wait_for("wc: wierszy=1 bajtow=2"))

        # head ucina do N wierszy: seq 20 | head 3 -> 3 wiersze, 6 bajtow
        q.type_line("seq 20 | head 3 > h.txt")
        time.sleep(0.5)
        q.type_line("cat h.txt | wc")
        check("head ogranicza potok do N wierszy",
              "wc: wierszy=3 bajtow=6" in q.wait_for("wc: wierszy=3 bajtow=6"))

        # Potok 3-stopniowy: seq 20 | grep 1 | wc. Liczby 1..20 z cyfra
        # '1' to 1,10..19 = 11 wierszy; bajty 2 + 10*3 = 32
        q.type_line("seq 20 | grep 1 | wc")
        check("potok 3-stopniowy (seq | grep | wc)",
              "wc: wierszy=11 bajtow=32" in q.wait_for("wc: wierszy=11"))

        # Potok 4-stopniowy z przekierowaniem: seq 30 | grep 2 | head 3 > p.txt
        # liczby z '2': 2,12,20,...; pierwsze 3 -> "2\n12\n20\n" = 3w/8B
        q.type_line("seq 30 | grep 2 | head 3 > p.txt")
        time.sleep(0.5)
        q.type_line("cat p.txt | wc")
        check("potok 4-stopniowy z przekierowaniem do pliku",
              "wc: wierszy=3 bajtow=8" in q.wait_for("wc: wierszy=3 bajtow=8"))

        # ls/rm jako programy ring 3. 'echo xyz > usuwany' -> "xyz\n" (4 B);
        # ls pokazuje "usuwany  4 B" (rozmiar nie wystepuje w wpisanej
        # komendzie, wiec to jednoznaczny dowod)
        q.type_line("echo xyz > usuwany")
        time.sleep(0.5)
        q.type_line("ls")
        check("ls (ring 3) listuje plik z rozmiarem",
              "usuwany  4 B" in q.wait_for("usuwany  4 B"))
        # po rm plik znika - sprawdzamy deterministycznie przez potok
        q.type_line("rm usuwany")
        time.sleep(0.5)
        q.type_line("ls | grep usuwany | wc")
        check("rm usunal plik (ls juz go nie pokazuje)",
              "wc: wierszy=0 bajtow=0" in q.wait_for("wc: wierszy=0 bajtow=0"))

        # Kody wyjscia: grep bez dopasowan konczy sie kodem 1, a kod
        # ostatniego etapu potoku wraca z wait do shella
        q.type_line("seq 5 | grep 9")
        check("kod wyjscia z potoku (grep bez dopasowan -> kod 1)",
              "ush: [kod 1]" in q.wait_for("ush: [kod 1]"))

        # Proces ubity wyjatkiem CPU zwraca kod -1
        q.type_line("evil")
        check("kod wyjscia procesu ubitego wyjatkiem (-1)",
              "ush: [kod -1]" in q.wait_for("ush: [kod -1]"))

        # RTC: 'date' (ring 3) czyta zegar CMOS; QEMU ma stala baze.
        # Sprawdzamy tylko date (godzina przesunie sie przez czas testu),
        # plus format 'godz:min:sek' (dwie cyfry kazda)
        q.type_line("date")
        screen = q.wait_for(RTC_DATE)
        m = re.search(RTC_DATE + r" (\d\d):(\d\d):(\d\d)", screen)
        check("date (ring 3) czyta RTC: data i format z bazy QEMU",
              m is not None)

        # Dopisywanie '>>': apf = "appendtest\n"(11B) + "zz\n"(3B) = 14B/2w.
        # Gdyby '>>' obcinalo jak '>', bylby tylko "zz\n" (3B/1w).
        q.type_line("echo appendtest > apf")
        time.sleep(0.4)
        q.type_line("echo zz >> apf")
        time.sleep(0.4)
        q.type_line("cat apf | wc")
        check("dopisywanie '>>' nie obcina pliku (14 B, 2 wiersze)",
              "wc: wierszy=2 bajtow=14" in q.wait_for("wc: wierszy=2 bajtow=14"))

        # cp tworzy NIEZALEZNA kopie: dopisanie do kopii (+ "more\n" 5B)
        # daje 19B, nie ruszajac oryginalu
        q.type_line("cp apf cpf")
        time.sleep(0.4)
        q.type_line("echo more >> cpf")
        time.sleep(0.4)
        q.type_line("cat cpf | wc")
        check("cp tworzy niezalezna kopie pliku (19 B, 3 wiersze)",
              "wc: wierszy=3 bajtow=19" in q.wait_for("wc: wierszy=3 bajtow=19"))

        # mv przenosi: zrodlo znika, cel ma tresc
        q.type_line("mv cpf mvf")
        time.sleep(0.4)
        q.type_line("cat cpf")
        check("mv usunal plik zrodlowy",
              "nie moge otworzyc 'cpf'" in q.wait_for("nie moge otworzyc 'cpf'"))
        q.type_line("echo end >> mvf")
        time.sleep(0.4)
        q.type_line("cat mvf | wc")
        check("mv zachowal tresc w pliku docelowym (23 B, 4 wiersze)",
              "wc: wierszy=4 bajtow=23" in q.wait_for("wc: wierszy=4 bajtow=23"))

        # Katalogi: mkdir + cd (prompt pokazuje sciezke roboczy)
        q.type_line("mkdir d1")
        time.sleep(0.4)
        q.type_line("cd d1")
        check("cd wchodzi do podkatalogu (prompt = sciezka robocza)",
              "ush:/d1>" in q.wait_for("ush:/d1>"))
        # plik tworzony w podkatalogu (wzgledem cwd); "hejka\n" = 6 B
        q.type_line("echo hejka > f1")
        time.sleep(0.4)
        q.type_line("cd ..")
        time.sleep(0.4)
        # podkatalog widoczny w katalogu glownym jako 'd1/'
        q.type_line("ls")
        check("ls pokazuje podkatalog ze slashem (d1/)",
              "d1/" in q.wait_for("d1/"))
        # plik z podkatalogu osiagalny sciezka wzgledna; nie ma go w root
        q.type_line("cat d1/f1 | wc")
        check("sciezka wzgledna do pliku w podkatalogu (6 B, 1 wiersz)",
              "wc: wierszy=1 bajtow=6" in q.wait_for("wc: wierszy=1 bajtow=6"))

        # Tlo ('&') + kill: spin dziala w tle, kill go konczy
        q.type_line("spin &")
        sbg = q.wait_for("w tle, pid=")
        pidm = re.search(r"w tle, pid=(\d+)", sbg)
        check("uruchomienie w tle ('&') zwraca pid", pidm is not None)
        time.sleep(1.0)
        q.type_line("kill " + pidm.group(1))
        check("kill konczy proces w tle (komunikat 'zabity')",
              "zabity (kod -9)" in q.wait_for("zabity (kod -9)"))
        # po zabiciu spin nie produkuje juz znacznikow
        base = q.screen().count("[spin] tik")
        time.sleep(1.6)
        check("zabity proces przestal dzialac (brak nowych tikow)",
              q.screen().count("[spin] tik") == base)

        q.type_line("exit")
        view = q.wait_for("ush: pa!")
        check("ush wyszedl przez sys_exit", "ush: pa!" in view)

        # Konsola wraca do shella jadra
        before = len(re.findall(r"tyknac timera", q.screen()))
        q.type_line("uptime")
        time.sleep(0.3)
        after = len(re.findall(r"tyknac timera", q.screen()))
        check("konsola wrocila do shella jadra po smierci ush",
              after > before or "tyknac timera" in q.screen() and before == 0)

        # System musi zyc dalej: liczniki rosna, shell odpowiada
        a3, _ = counters(q.screen())
        time.sleep(1)
        a4, _ = counters(q.screen())
        check(f"system zyje po ubiciu procesu ({a3} -> {a4})",
              a3 is not None and a4 is not None and a4 > a3)
        q.type_line("uptime")
        check("shell odpowiada po ubiciu procesu",
              "tyknac timera" in q.screen())
    finally:
        q.close()
        if os.path.exists(disk):
            os.unlink(disk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
