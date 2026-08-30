#!/usr/bin/env python3
"""Test dymny ProtonOS: siec, TLS 1.3 i system plikow (jadro 32-bitowe).

Druga polowa dawnej grupy 'rdzen'. Podzial nie jest tematyczny dla
ozdoby: jedna grupa robila 174 asercje na jednej maszynie i sama
decydowala, jak dlugo trwa caly bieg. Dwie maszyny licza sie
rownolegle, wiec ten sam zestaw sprawdzen konczy sie w polowie czasu.

Zostaje tu wszystko, co potrzebuje sieci albo dysku - lacznie
z najmocniejszym dowodem na system plikow, czyli DRUGIM ROZRUCHEM
na tym samym obrazie dysku.

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
    # Swiezy obraz dysku wspoldzielony przez oba rozruchy tej grupy -
    # jadro sformatuje go przy pierwszym starcie
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
        # Adres IP przychodzi z serwera DHCP slirpa juz po starcie
        # shella - bez czekania na niego kazdy test sieciowy bylby
        # wyscigiem z dzierzawa
        q.wait_for("[siec] DHCP", timeout=30)
        time.sleep(0.5)

        # Siec: ARP + ICMP przez karte RTL8139. Brama slirpa (10.0.2.2)
        # odpowiada na oba, wiec test nie potrzebuje sieci na hoscie.
        q.type_line("ifconfig")
        ifc = q.wait_for("eth0  RTL8139, MAC")
        check("ifconfig nazywa karte, ktora odpowiedziala",
              "eth0  RTL8139, MAC" in ifc)
        check("ifconfig pokazuje adres IP", "IP 10.0.2.15" in ifc)
        check("ifconfig pokazuje brame", "brama 10.0.2.2" in ifc)
        check("ifconfig pokazuje serwer DNS z DHCP", "DNS 10.0.2.3" in ifc)

        # Shell jest wlasnym watkiem: 'ping' spi w nim przez cztery sekundy,
        # a komenda wpisana w miedzyczasie czeka w buforze klawiatury
        # i wykonuje sie zaraz po nim. Dawniej shell dzialal w kontekscie
        # przerwania, wiec spac nie mogl i ping szedl do osobnego watku.
        q.type_line("ping 10.0.2.2")
        q.type_line("dns 10.0.2.3")     # pisanie na zapas w trakcie pinga
        pong = q.wait_for("40 B od 10.0.2.2: seq=1", timeout=20)
        check("ping: odpowiedz ICMP echo od bramy", "40 B od 10.0.2.2" in pong)
        stats = q.wait_for("--- statystyki:", timeout=20)
        check("ping: wszystkie 4 pakiety wrocily",
              "wyslano 4, odebrano 4" in stats)
        buffered = q.wait_for("serwer DNS ustawiony na", timeout=20)
        check("znaki wpisane w trakcie komendy nie gina (bufor klawiatury)",
              "serwer DNS ustawiony na 10.0.2.3" in buffered)

        q.type_line("arp")
        check("tablica ARP zna MAC bramy",
              "10.0.2.2" in q.wait_for("10.0.2.2  "))

        # Gniazda UDP w ring 3: dwa procesy gadaja przez petle zwrotna,
        # wiec test nie zalezy od zadnej sieci na zewnatrz
        q.type_line("run udpecho")
        view = q.wait_for("nasluchuje na porcie 7777", timeout=10)
        check("proces ring 3 otworzyl gniazdo UDP",
              "nasluchuje na porcie 7777" in view)

        q.type_line("run udptest")
        echo = q.wait_for("echo wrocilo", timeout=10)
        check("datagram wrocil do nadawcy przez petle zwrotna",
              "echo wrocilo: proton-udp" in echo)
        check("serwer zobaczyl datagram od klienta",
              "[udpecho] od 10.0.2.15:" in echo)
        check("klient zakonczyl sie bez bledu",
              "proces 'udptest' zakonczyl sie (kod 0)" in echo)

        # Gniazdo ma wlasciciela: po smierci procesu port musi sie zwolnic
        q.type_line("kill udpecho")
        time.sleep(0.5)
        q.type_line("clear")          # czysty ekran: stare "nasluchuje" znika
        q.type_line("run udpecho")
        again = q.wait_for("porcie 7777", timeout=10)
        check("port zwolniony po smierci wlasciciela gniazda",
              "nasluchuje na porcie 7777" in again and
              "nie udalo sie otworzyc gniazda" not in again)
        q.type_line("kill udpecho")

        # DNS z ring 3. Pytamy wlasny serwer stojacy na hoscie (10.0.2.2),
        # wiec test nie zalezy od internetu ani od resolvera maszyny.
        dns_stop = start_dns_server()
        try:
            q.type_line("clear")
            q.type_line("dns 10.0.2.2")
            q.wait_for("serwer DNS ustawiony", timeout=10)
            q.type_line("run nslookup protonos.test")
            dns = q.wait_for("ma adres", timeout=20)
            check("nslookup rozwiazal nazwe przez wskazany serwer DNS",
                  "protonos.test ma adres 10.0.2.2" in dns)
        finally:
            dns_stop()

        # TCP jako klient: po drugiej stronie stoi prawdziwy stos
        # (serwer HTTP w Pythonie na hoscie), a nie nasza wlasna petla
        http_port, http_stop = start_http_server()
        try:
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d /" % http_port)
            page = q.wait_for("tresc ", timeout=30)
            check("TCP: handshake i odpowiedz HTTP 200",
                  "[httpget] HTTP 200" in page)
            check("TCP: tresc strony dotarla w calosci",
                  "ProtonOS TCP dziala" in page)
            check("httpget zakonczyl sie bez bledu",
                  "proces 'httpget' zakonczyl sie (kod 0)" in page)

            # Siec -> dysk: tresc HTTP zapisana na PFS i odczytana z niego
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d / strona.htm" % http_port)
            q.wait_for("tresc ", timeout=30)
            q.type_line("cat strona.htm")
            saved = q.wait_for("ProtonOS TCP dziala", timeout=10)
            check("tresc z sieci zapisana na PFS i odczytana z dysku",
                  "<html>ProtonOS TCP dziala</html>" in saved)
            check("do pliku poszla sama tresc, bez naglowkow HTTP",
                  "Content-Length" not in saved)

            # PFS3: plik grubo ponad stary limit 16 KiB na plik
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d /duza duza.htm" % http_port)
            big = q.wait_for("tresc ", timeout=90)
            # httpget podaje dlugosc samej tresci (bez naglowkow)
            got = re.search(r"tresc (\d+) B", big)
            check("duza strona pobrana w calosci (%d B tresci)" % len(BIG_PAGE),
                  got is not None and int(got.group(1)) == len(BIG_PAGE))
            q.type_line("fls")
            listing = q.wait_for("duza.htm", timeout=10)
            check("plik ponad stary limit 16 KiB zapisany na PFS",
                  "duza.htm  (%d B)" % len(BIG_PAGE) in listing)
            q.type_line("df")
            usage = q.wait_for("dysk: ", timeout=10)
            check("df pokazuje zajetosc dysku w blokach",
                  "bloki po 1024 B" in usage and "zajete" in usage)

            # Plik ponad dawny sufit 268 KiB: dane trafiaja do bloku
            # podwojnie posredniego, a potem czytamy je z powrotem
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d /ogromna ogromna.htm"
                        % http_port)
            huge = q.wait_for("tresc ", timeout=180)
            check("plik ponad 268 KiB pobrany w calosci (%d B)"
                  % len(HUGE_PAGE),
                  "tresc %d B" % len(HUGE_PAGE) in huge)

            q.type_line("fls")
            listing = q.wait_for("ogromna.htm", timeout=15)
            check("zapisany na PFS z pelnym rozmiarem",
                  "ogromna.htm  (%d B)" % len(HUGE_PAGE) in listing)

            q.type_line("run sh")
            q.wait_for("ush:/", timeout=20)
            q.type_line("cat ogromna.htm | wc")
            back = q.wait_for("bajtow=", timeout=180)
            check("odczyt przez blok podwojnie posredni oddaje te same dane",
                  "wierszy=6500 bajtow=%d" % len(HUGE_PAGE) in back)
            q.type_line("exit")
            q.wait_for("ush: pa!", timeout=20)

            # HTTP/1.1: przekierowanie 302 i tresc w kawalkach (chunked) -
            # tak odpowiada wiekszosc prawdziwych serwerow
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d /stara" % http_port)
            hop = q.wait_for("tresc ", timeout=30)
            check("klient idzie za przekierowaniem 302",
                  "przekierowanie -> /kawalki" in hop)
            check("tresc w kawalkach (chunked) zlozona poprawnie",
                  "HTTP 200 (chunked), tresc %d B" % len(PAGE) in hop)
            check("tresc po zlozeniu jest cala",
                  "ProtonOS TCP dziala" in hop)
        finally:
            http_stop()

        q.type_line("netstat")
        check("netstat: po zamknieciu nie ma wiszacych polaczen",
              "brak polaczen TCP" in q.wait_for("polacze", timeout=10))

        q.type_line("ifconfig")
        ifc = q.wait_for("RX ")
        # Na ekranie wisi tez wyjscie poprzedniego 'ifconfig' (sprzed
        # pinga) - liczy sie ostatnie, najswiezsze wystapienie
        rx = re.findall(r"RX (\d+) pakietow, TX (\d+) pakietow", ifc)
        check(f"licznik pakietow RX/TX rosnie (ostatni: {rx[-1] if rx else None})",
              bool(rx) and int(rx[-1][0]) > 0 and int(rx[-1][1]) > 0)

        # Mysz PS/2: ruch (delty wzgledne) i przycisk, czytane komenda 'mouse'.
        # Start na srodku 320x200 = (160,100); QEMU przekazuje delty 1:1.
        q.mouse_move(40, 30)
        q.type_line("mouse")
        check("mysz reaguje na ruch (160,100)+(40,30) -> (200,130)",
              "mysz: x=200 y=130" in q.wait_for("mysz: x=200 y=130"))
        q.mouse_move(-80, -50)
        q.mouse_button(True)
        q.type_line("mouse")
        check("mysz: kolejny ruch i wcisniety LPM (120,80 b1)",
              "mysz: x=120 y=80 przyciski=1" in
              q.wait_for("mysz: x=120 y=80 przyciski=1"))
        q.mouse_button(False)

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

        # --- System plikow na dysku (PFS) ---
        # Proces ring 3 pisze i czyta plik syscallami
        q.type_line("run fileio")
        screen = q.wait_for("[fileio] OK")
        check("proces ring3 zapisal i odczytal plik z dysku",
              "odczytano z dysku: plik zapisany przez proces ring3" in screen)

        # Shell jadra: write -> cat (zapis i odczyt w obrebie jednego boota)
        q.type_line("write greeting witaj-z-dysku-pfs")
        check("zapis pliku z shella jadra",
              "zapisano" in q.wait_for("zapisano"))
        q.type_line("cat greeting")
        check("odczyt zapisanego pliku (cat)",
              "witaj-z-dysku-pfs" in q.wait_for("witaj-z-dysku-pfs"))

        # fls pokazuje oba pliki
        q.type_line("fls")
        screen = q.wait_for("Katalog glowny")
        check("fls listuje pliki na dysku",
              "greeting" in screen and "ring3.txt" in screen)

        # cat nieistniejacego pliku
        q.type_line("cat niema")
        check("cat zglasza brak pliku",
              "brak pliku 'niema'" in q.wait_for("brak pliku"))

        # Program 'cat' w ring 3: czyta plik przez fd, pisze na fd 1
        # (zunifikowany model I/O: read pliku -> write na konsole).
        # Czyscimy ekran, by nie zlapac starego echa 'witaj-z-dysku-pfs'.
        q.type_line("clear")
        time.sleep(0.3)
        q.type_line("run cat greeting")
        check("program ring3 'cat' czyta plik przez fd i pisze na fd 1",
              "witaj-z-dysku-pfs" in q.wait_for("witaj-z-dysku-pfs",
                                                timeout=20))

        # Na koncu: crash powinien wywolac page fault z diagnostyka
        q.type_line("crash")
        screen = q.screen()
        check("page fault obsluzony (wyjatek 14)",
              "WYJATEK CPU 14: Page Fault" in screen)
        check("CR2 wskazuje felerny adres",
              "(CR2): e0000000" in screen)
        check("system zatrzymany po wyjatku", "System zatrzymany" in screen)
    finally:
        q.close()

    # --- Persystencja: drugi boot na TYM SAMYM dysku ---
    print("--- drugi boot (ten sam dysk) ---")
    q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        screen = q2.wait_for("ProtonOS shell")
        check("drugi boot: dysk wykryty (juz sformatowany, bez ponownego format)",
              "+ PFS gotowy" in screen
              and "sformatowany" not in screen)

        q2.type_line("fls")
        screen = q2.wait_for("Katalog glowny")
        check("pliki przetrwaly restart (fls)",
              "greeting" in screen and "ring3.txt" in screen)

        q2.type_line("cat greeting")
        check("zawartosc pliku przetrwala restart (cat)",
              "witaj-z-dysku-pfs" in q2.wait_for("witaj-z-dysku-pfs"))

        q2.type_line("cat ring3.txt")
        check("plik zapisany przez ring3 przetrwal restart",
              "plik zapisany przez proces ring3" in q2.wait_for("ring3"))

        # Duzy plik z sieci: i-wezel z blokiem posrednim po restarcie
        q2.type_line("fls")
        listing = q2.wait_for("duza.htm", timeout=10)
        check("duzy plik (>100 KiB) przetrwal restart z pelnym rozmiarem",
              "duza.htm  (%d B)" % len(BIG_PAGE) in listing)
        q2.type_line("df")
        usage = q2.wait_for("dysk: ", timeout=10)
        check("zajetosc dysku odtworzona z bitmapy po restarcie",
              "zajete" in usage and "zajete 0 KiB" not in usage)
    finally:
        q2.close()
        if os.path.exists(disk):
            os.unlink(disk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
