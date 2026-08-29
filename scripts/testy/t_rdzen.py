#!/usr/bin/env python3
"""Test dymny ProtonOS: boot, shell, procesy, pamiec, system plikow, siec, TLS.

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
from ptest import check, wynik


def main():
    # Swiezy obraz dysku (1 MiB) wspoldzielony przez oba boot-y -
    # jadro sformatuje go przy pierwszym starcie
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        # Czekaj na pelny boot (prompt shella) zamiast zgadywac czas
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

        # Adres IP przychodzi z serwera DHCP slirpa juz po starcie shella.
        # Czekamy na komunikat, zanim ekran zacznie sluzyc testom
        # klawiatury - inaczej wpada w srodek pisanej linii.
        lease = q.wait_for("[siec] DHCP", timeout=20)
        # Komunikat sklada sie z kilku printk-ow, wiec chwila na dopisanie
        # reszty linii - inaczej test czyta ja w polowie
        time.sleep(0.5)
        lease = q.screen()
        check("DHCP przyznal adres IP", "przyznano 10.0.2.15" in lease)
        check("DHCP przekazal brame i serwer DNS",
              "brama 10.0.2.2" in lease and "DNS 10.0.2.3" in lease)

        for expected in ["Multiboot OK", "GDT zaladowana", "IDT zaladowana",
                         "PIC przemapowany", "Timer PIT", "Klawiatura PS/2",
                         "Przerwania wlaczone"]:
            check(f"komunikat '{expected}'", expected in screen)

        check("obsluga wyjatku int3 (breakpoint)", "[INT3]" in screen)

        for expected in ["Pamiec fizyczna", "Stronicowanie wlaczone",
                         "kmalloc/kfree dzialaja"]:
            check(f"komunikat '{expected}'", expected in screen)

        check("jadro przezylo init pamieci (doszlo do shella)",
              "ProtonOS shell" in screen)

        # Timer: uptime w rogu ekranu musi urosnac
        def uptime(s):
            m = re.search(r"uptime: (\d+)s", s)
            return int(m.group(1)) if m else None

        t1 = uptime(q.wait_for("uptime: "))
        time.sleep(2)
        t2 = uptime(q.screen())
        check(f"timer PIT tyka (uptime {t1}s -> {t2}s)",
              t1 is not None and t2 is not None and t2 > t1)

        # Wielozadaniowosc: liczniki watkow A i B musza rosnac niezaleznie.
        # Bierzemy OSTATNIE wystapienie - po przewinieciu ekranu wyzej
        # moga wisiec stare, martwe kopie linii z licznikami.
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

        # Watek C konczy sie po 2 s
        screen = q.wait_for("watek-C: zakonczony")
        check("watek C zakonczyl prace", "watek-C: zakonczony" in screen)

        # Klawiatura: wstrzyknij "proton 1" i sprawdz echo
        for key in ["p", "r", "o", "t", "o", "n", "spc", "1"]:
            q.sendkey(key)
        q.sendkey("shift-x")  # test shifta: powinno wyjsc 'X'
        screen = q.screen()
        check("echo klawiatury: 'proton 1'", "> proton 1" in screen)
        check("echo klawiatury z shiftem: 'X'", "> proton 1X" in screen)

        # Dokoncz smieciowa linie enterem i przetestuj komendy shella
        q.sendkey("ret")
        time.sleep(0.3)
        screen = q.screen()
        check("nieznana komenda zgloszona",
              "nieznana komenda: 'proton'" in screen)

        q.type_line("help")
        screen = q.screen()
        check("komenda 'help' wypisuje liste komend",
              "Dostepne komendy" in screen and "reboot" in screen)

        q.type_line("mem")
        screen = q.screen()
        check("komenda 'mem' pokazuje RAM i ramki",
              "RAM wykryty" in screen and "ramek 4 KiB" in screen)
        check("komenda 'mem' pokazuje sterte", "sterta jadra" in screen)
        # Cala wykryta pamiec jest pod zarzadem, nie tylko pierwsze 64 MiB:
        # mapowanie identycznosciowe ciagnie sie az po szczyt RAM-u.
        mm = re.search(r"zarzadzane (\d+) MiB", screen)
        ram = re.search(r"RAM wykryty:\s+(\d+) KiB", screen)
        check("zarzadzana pamiec siega szczytu wykrytego RAM-u"
              + (f" ({mm.group(1)} MiB z {int(ram.group(1)) // 1024} MiB)"
                 if mm and ram else ""),
              mm is not None and ram is not None
              and int(mm.group(1)) >= int(ram.group(1)) // 1024 - 2)

        q.type_line("uptime")
        screen = q.screen()
        check("komenda 'uptime' dziala", "tyknac timera" in screen)

        q.type_line("ps")
        screen = q.screen()
        check("komenda 'ps' pokazuje watki",
              "main" in screen and "demo-A" in screen
              and "demo-B" in screen)
        check("shell jest wlasnym watkiem (nie kontekst przerwania)",
              "shell (biezacy)" in screen)
        check("zombie po watku C zostal sprzatniety",
              "demo-C" not in screen)

        # initrd: 'ls' pokazuje spakowane programy. Plikow jest duzo,
        # wiec wczesne (hello/evil) sie przewijaja - sprawdzamy pozne,
        # ktore zostaja na dole ekranu.
        q.type_line("ls")
        screen = q.screen()
        check("komenda 'ls' pokazuje pliki initrd",
              "gmon" in screen and "shmread" in screen)

        # Wyciek ramek: zmierz uzycie PMM przed i po zyciu procesu
        def pmm_used(s):
            m = re.findall(r"PMM:\s+(\d+)/", s)
            return int(m[-1]) if m else None

        q.type_line("mem")
        used_before = pmm_used(q.screen())

        # 'run hello': proces ELF z wlasna przestrzenia adresowa
        q.type_line("run hello")
        screen = q.wait_for("[hello] czesc, jestem procesem ELF")
        check("proces hello wystartowal z ELF i robi syscall write",
              "[hello] czesc, jestem procesem ELF" in screen)
        # Przestrzen uzytkownika zaczyna sie od 1 GiB - ponizej jest juz
        # tylko mapowanie identycznosciowe jadra
        check("program uzytkownika mieszka powyzej 1 GiB",
              re.search(r"entry 4[0-9a-f]{7}", screen) is not None)
        widok = q.wait_for("[hello] licze: 3")
        check("syscalle write+sleep dzialaja w petli",
              "[hello] licze: 3" in widok)
        widok = q.wait_for("proces 'hello' zakonczyl sie (kod 0)")
        check("hello zakonczyl sie czysto przez sys_exit",
              "proces 'hello' zakonczyl sie (kod 0)" in widok)

        time.sleep(0.5)  # czas na sprzatniecie zombie
        q.type_line("mem")
        used_after = pmm_used(q.screen())
        check(f"brak wycieku ramek po procesie ({used_before} -> {used_after})",
              used_before is not None and used_after == used_before)

        # 'run echo ...': argumenty (argc/argv) docieraja do procesu
        q.type_line("run echo proton os dziala")
        screen = q.wait_for("proton os dziala")
        check("argumenty argc/argv docieraja do procesu (echo)",
              "proton os dziala" in screen)

        # 'run memtest': sbrk/malloc daje procesowi rosnaca pamiec
        q.type_line("run memtest")
        screen = q.wait_for("[memtest] OK:")
        check("sbrk/malloc dziala (memtest zapisal i zweryfikowal pamiec)",
              "blokow po" in screen and "zweryfikowanych" in screen)
        check("memtest nie zglosil bledu pamieci",
              "BLAD" not in screen.split("[memtest] OK")[0].split("[memtest] start")[-1])

        # Brak wycieku takze po procesie uzywajacym sbrk
        time.sleep(0.5)
        q.type_line("mem")
        used_after_mt = pmm_used(q.screen())
        check(f"brak wycieku ramek po memtest ({used_before} -> {used_after_mt})",
              used_after_mt is not None and used_after_mt == used_before)

        # 'run mmaptest': mmap daje swieza, wyzerowana, zapisywalna pamiec
        q.type_line("run mmaptest")
        screen = q.wait_for("[mmaptest] OK:")
        check("mmap dziala (3 strony zmapowane, zerowe i zapisywalne)",
              "zapisane i zweryfikowane" in screen)
        # i nie wycieka po zakonczeniu procesu (zwolnione z przestrzenia)
        time.sleep(0.5)
        q.type_line("mem")
        used_after_mm = pmm_used(q.screen())
        check(f"brak wycieku ramek po mmaptest ({used_before} -> {used_after_mm})",
              used_after_mm is not None and used_after_mm == used_before)

        # 'run shmtest': pamiec wspoldzielona miedzy procesami. shmtest
        # zapisuje 12345 i uruchamia shmread (osobny proces), ktory czyta
        # te sama pamiec - dowodem jest jego linia z odczytana wartoscia.
        q.type_line("run shmtest")
        screen = q.wait_for("[shmread] wartosc=")
        check("pamiec wspoldzielona: inny proces czyta zapisana wartosc",
              "[shmread] wartosc=12345" in screen)

        # 'run threads': dwa watki w jednym procesie (wspolna pamiec).
        # Kazdy zapisuje inna zmienna globalna; main sumuje -> 33.
        # Swieza baza ramek (shmtest zajal trwaly region wspoldzielony).
        q.type_line("mem")
        used_before_th = pmm_used(q.screen())
        q.type_line("run threads")
        screen = q.wait_for("[threads] watki zapisaly")
        check("watki w procesie dziela pamiec (suma globalnych = 33)",
              "suma=33" in screen)
        # brak wycieku ramek po procesie wielowatkowym (stosy watkow +
        # przestrzen adresowa zwolnione, gdy konczy sie ostatni watek)
        time.sleep(0.5)
        q.type_line("mem")
        used_after_th = pmm_used(q.screen())
        check(f"brak wycieku ramek po watkach ({used_before_th} -> {used_after_th})",
              used_after_th is not None and used_after_th == used_before_th)

        # 'run forktest': fork() z kopiowaniem przy zapisie. Program
        # zajmuje 256 KiB, rozwidla sie i sprawdza, ze dziecko dostalo
        # wzorzec rodzica, a wlasne zapisy zostaly u niego - przy czym
        # tuz po fork pamiec urosla o tablice stron, nie o caly bufor.
        q.type_line("clear")
        q.type_line("mem")
        used_before_fk = pmm_used(q.screen())
        q.type_line("run forktest")
        screen = q.wait_for("[forktest] dziecko: wzorzec odziedziczony",
                            timeout=30)
        check("fork: dziecko wraca z syscalla z pamiecia rodzica",
              "[forktest] dziecko: wzorzec odziedziczony" in screen)
        m = re.search(r"przyrost RAM po fork: (\d+) KiB", screen)
        check("fork nie kopiuje pamieci procesu"
              + (f" (256 KiB bufora, przyrost {m.group(1)} KiB)" if m else ""),
              m is not None and int(m.group(1)) < 64)
        screen = q.wait_for("[forktest] rodzic: wzorzec nietkniety",
                            timeout=30)
        check("zapisy dziecka nie widac u rodzica (prywatne kopie stron)",
              "[forktest] rodzic: wzorzec nietkniety" in screen)
        # Ten sam wzorzec zostal nietkniety takze tam, gdzie w dziecku
        # pisalo JADRO (syscall) - o to dba bit WP w CR0.
        check("zapis jadra w dziecku tez trafil w prywatna kopie strony",
              "rodzic: wzorzec nietkniety" in screen)
        check("rodzic odbiera kod wyjscia dziecka przez wait",
              "kod dziecka=7" in screen)
        # Druga polowa pary uniksowej: dziecko podmienia sie w inny
        # program przez exec, a rodzic czeka na ten sam pid.
        screen = q.wait_for("[forktest] rodzic doczekal sie pidu", timeout=30)
        check("exec podmienia obraz procesu w miejscu",
              "[forktest] po exec: inny program, ten sam pid" in screen)
        check("po exec proces nazywa sie jak nowy program",
              "proces 'echo' zakonczyl sie (kod 0)" in screen)
        check("wait rodzica trafia w ten sam pid po exec",
              "doczekal sie pidu" in screen)
        q.wait_for("proces 'forktest' zakonczyl sie (kod 0)", timeout=20)
        time.sleep(0.6)
        q.type_line("mem")
        used_after_fk = pmm_used(q.screen())
        check(f"brak wycieku ramek po fork ({used_before_fk} -> {used_after_fk})",
              used_after_fk is not None and used_after_fk == used_before_fk)

        # 'run lazymem': stronicowanie na zadanie. Prosba o 64 MiB nie
        # kosztuje ani ramki - place sie dopiero za dotkniete strony.
        q.type_line("clear")
        time.sleep(0.6)          # niech zombie po poprzednim tescie zniknie
        q.type_line("mem")
        used_before_lz = pmm_used(q.screen())
        q.type_line("run lazymem")
        screen = q.wait_for("[lazymem] dotkniete", timeout=40)
        m = re.search(r"poproszone 64 MiB, RAM urosl o (\d+) KiB", screen)
        check("prosba o 64 MiB sterty nie kosztuje ani ramki"
              + (f" (+{m.group(1)} KiB)" if m else ""),
              m is not None and int(m.group(1)) == 0)
        m = re.search(r"dotkniete 1 MiB, RAM urosl o (\d+) KiB", screen)
        check("place sie dokladnie za dotkniete strony"
              + (f" (+{m.group(1)} KiB)" if m else ""),
              m is not None and int(m.group(1)) == 1024)
        screen = q.wait_for("[lazymem] mmap tez daje strony", timeout=30)
        check("syscall urzeczywistnia nietknieta strone zamiast zglaszac blad",
              "syscall urzeczywistnil nietknieta strone" in screen)
        check("mmap tez przydziela strony dopiero przy dotknieciu",
              "[lazymem] mmap tez daje strony" in screen)
        q.wait_for("proces 'lazymem' zakonczyl sie (kod 0)", timeout=20)
        time.sleep(0.6)
        q.type_line("mem")
        used_after_lz = pmm_used(q.screen())
        check(f"brak wycieku ramek po stronicowaniu na zadanie"
              f" ({used_before_lz} -> {used_after_lz})",
              used_after_lz is not None and used_after_lz == used_before_lz)

        # 'run ptree': drzewo procesow. Trzy dzieci koncza sie w innej
        # kolejnosci, niz powstaly, a rodzic zbiera je przez waitany.
        q.type_line("clear")
        q.type_line("mem")
        used_before_pt = pmm_used(q.screen())
        q.type_line("run ptree")
        screen = q.wait_for("[ptree] zebrane wszystkie dzieci", timeout=30)
        m = re.findall(r"skonczylo dziecko (\d+) z kodem (\d+)", screen)
        check("wait na dowolne dziecko oddaje je w kolejnosci konczenia"
              + (f" (pidy {[p for p, _ in m]})" if m else ""),
              len(m) == 3 and [int(p) for p, _ in m] == sorted(
                  (int(p) for p, _ in m), reverse=True))
        check("kod wyjscia pasuje do dziecka, ktore go zwrocilo",
              [c for _, c in m] == ["22", "21", "20"])
        screen = q.wait_for("[ptree] sierota przygarnieta", timeout=30)
        check("sierota trafia pod opieke dziadka (kod 99 dociera do niego)",
              "z kodem 99" in screen)
        q.wait_for("proces 'ptree' zakonczyl sie (kod 0)", timeout=20)
        time.sleep(0.6)
        q.type_line("mem")
        used_after_pt = pmm_used(q.screen())
        check(f"brak wycieku ramek po drzewie procesow"
              f" ({used_before_pt} -> {used_after_pt})",
              used_after_pt is not None and used_after_pt == used_before_pt)

        q.type_line("clear")
        q.type_line("ps")
        screen = q.screen()
        check("ps pokazuje rodzica kazdego watku",
              "RODZIC" in screen and re.search(r"\n  0   -", screen) is not None)

        # 'run sigtest': sygnaly. Najostrzejszy punkt to powrot
        # z procedury obslugi - sygnal przychodzi dokladnie w chwili,
        # gdy jadro odlozylo juz wynik przerwanego syscalla.
        q.type_line("clear")
        q.type_line("mem")
        used_before_sg = pmm_used(q.screen())
        q.type_line("run sigtest")
        screen = q.wait_for("[sigtest] wlasna procedura obslugi", timeout=30)
        check("wlasna procedura obslugi sygnalu dziala w ring 3 i wraca",
              "dostala SIGINT i wrocila" in screen)
        screen = q.wait_for("[sigtest] SIG_IGN", timeout=20)
        check("SIG_IGN gubi sygnal zamiast go doreczac",
              "sygnal przepadl" in screen)
        screen = q.wait_for("[sigtest] rodzic dostal SIGCHLD", timeout=30)
        check("SIGTERM domyslnie konczy proces (kod -15)",
              "domyslnie konczy proces (kod -15)" in screen)
        check("rodzic dostaje SIGCHLD po smierci dziecka",
              "[sigtest] rodzic dostal SIGCHLD" in screen)
        screen = q.wait_for("[sigtest] SIGKILL nie do przechwycenia",
                            timeout=30)
        check("SIGKILL-a nie da sie przechwycic ani zignorowac",
              "[sigtest] SIGKILL nie do przechwycenia" in screen)

        # Ctrl+C z prawdziwej klawiatury -> SIGINT do procesu, ktory
        # trzyma konsole; czekanie na klawisz przerywa sie w pol drogi
        q.wait_for("[sigtest] czekam na Ctrl+C", timeout=20)
        q.sendkey("ctrl-c")
        screen = q.wait_for("[sigtest] Ctrl+C dotarl", timeout=20)
        check("Ctrl+C dociera do procesu jako SIGINT i przerywa odczyt",
              "dotarl jako SIGINT" in screen)
        q.wait_for("proces 'sigtest' zakonczyl sie (kod 0)", timeout=20)
        time.sleep(0.6)
        q.type_line("mem")
        used_after_sg = pmm_used(q.screen())
        check(f"brak wycieku ramek po sygnalach"
              f" ({used_before_sg} -> {used_after_sg})",
              used_after_sg is not None and used_after_sg == used_before_sg)

        # Shell jadra umie wyslac dowolny sygnal, takze pod nazwe procesu
        q.type_line("clear")
        q.type_line("run spin")
        q.wait_for("[spin] tik", timeout=20)
        q.type_line("kill spin 15")
        screen = q.wait_for("zakonczony przez SIGTERM", timeout=20)
        check("'kill nazwa 15' z shella jadra wysyla SIGTERM",
              "kill: SIGTERM -> pid" in screen)

        # 'run prio': scheduler z priorytetami. Watek normalny wywlaszcza
        # niski -> przez to samo okno czasu naliczy wielokrotnie wiecej.
        q.type_line("run prio")
        screen = q.wait_for("[prio] normalny=", timeout=20)
        m = re.search(r"normalny=(\d+) niski=(\d+)", screen)
        ok = m and int(m.group(1)) > int(m.group(2)) * 5
        check("scheduler z priorytetami: normalny >> niski"
              + (f" ({m.group(1)} vs {m.group(2)})" if m else ""), ok)

        # 'run evil': proba czytania pamieci jadra
        q.type_line("run evil")
        screen = q.wait_for("proces 'evil' ubity")
        check("evil czytal pamiec jadra i zostal ubity",
              "proces 'evil' ubity" in screen)
        check("CR2 pokazuje atakowany adres",
              "siegnal po adres 100000" in screen)
        check("kod po ataku nie wykonal sie",
              "TEGO NIE POWINNO BYC WIDAC" not in screen)

        # 'run' z nieistniejacym plikiem
        # Granica jadro/uzytkownik: proces podaje w syscallach wskazniki
        # na pamiec jadra - kazdy musi wrocic z bledem, a proces ma zyc
        q.type_line("run evil2")
        ev2 = q.wait_for("granica trzyma", timeout=20)
        check("syscalle odrzucaja wskazniki na pamiec jadra",
              "jadro odrzucilo wszystkie proby" in ev2)
        check("evil2 nie wypisal pamieci jadra na konsole",
              "PRZESZLO" not in ev2 and "DZIURA" not in ev2)
        check("proces przezyl odrzucone syscalle (kod 0)",
              "kod 0" in ev2)

        # Kryptografia pod przyszly TLS: wektory z RFC liczone w ring 3
        q.type_line("clear")
        q.type_line("run cryptotest")
        crypto = q.wait_for("[crypto] wszystkie", timeout=20)
        check("wektory SHA-256/HMAC/HKDF/ChaCha20/Poly1305/X25519 z RFC",
              "wszystkie wektory zgadzaja sie" in crypto and
              "BLAD" not in crypto)

        q.type_line("run brak")
        check("run zgasza brak pliku", "brak pliku 'brak'" in q.screen())

        # Siec: ARP + ICMP przez karte RTL8139. Brama slirpa (10.0.2.2)
        # odpowiada na oba, wiec test nie potrzebuje sieci na hoscie.
        q.type_line("ifconfig")
        ifc = q.wait_for("eth0  MAC")
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
        widok = q.wait_for("nasluchuje na porcie 7777", timeout=10)
        check("proces ring 3 otworzyl gniazdo UDP",
              "nasluchuje na porcie 7777" in widok)

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
            # HTTPS: pelne uzgodnienie TLS 1.3 z prawdziwym OpenSSL-em po
            # drugiej stronie (X25519 + ChaCha20-Poly1305, sprawdzone Finished)
            https_port, https_stop = start_https_server()
            if https_port is None:
                print("INFO: brak openssl - pomijam testy TLS")
            else:
                dns_stop = start_dns_server()
                try:
                    q.type_line("dns 10.0.2.2")
                    q.wait_for("serwer DNS ustawiony", timeout=10)

                    # 1. Certyfikat samopodpisany, a w systemie tylko
                    #    wbudowane korzenie publicznych CA: polaczenie ma
                    #    zostac zerwane
                    q.type_line("clear")
                    q.type_line("run httpget https://protonos.test %d /"
                                % https_port)
                    rej = q.wait_for("nie udalo sie polaczyc", timeout=60)
                    check("obcy certyfikat odrzucony przez wbudowany "
                          "magazyn zaufania",
                          "brak zaufanego wystawcy" in rej)

                    # ... chyba ze swiadomie pominiemy sprawdzanie (-k)
                    q.type_line("clear")
                    q.type_line("run httpget -k https://protonos.test %d /"
                                % https_port)
                    sec = q.wait_for("tresc ", timeout=60)
                    check("TLS 1.3: uzgodnienie z serwerem OpenSSL",
                          "[httpget] TLS 1.3" in sec)
                    check("HTTPS: odpowiedz 200 z pelna trescia",
                          "HTTP 200, tresc %d B" % len(PAGE) in sec)
                    check("HTTPS: tresc odszyfrowana poprawnie",
                          "ProtonOS TCP dziala" in sec)
                    check("-k mowi wprost, ze tozsamosc nie jest sprawdzona",
                          "certyfikat NIESPRAWDZONY" in sec)

                    # 2. Pobranie certyfikatu wystawcy prosto na dysk
                    q.type_line("clear")
                    q.type_line("run httpget 10.0.2.2 %d /cert.der roots.der"
                                % http_port)
                    q.wait_for("tresc ", timeout=30)
                    q.type_line("fls")
                    check("magazyn zaufania zapisany na dysku",
                          "roots.der" in q.wait_for("roots.der", timeout=10))

                    # 3. Z magazynem: pelna weryfikacja lancucha, dat i nazwy
                    q.type_line("clear")
                    q.type_line("run httpget https://protonos.test %d /"
                                % https_port)
                    ver = q.wait_for("tresc ", timeout=60)
                    if "certyfikat sprawdzony" not in ver:
                        print("--- ekran (diagnostyka) ---")
                        print(ver)
                        print("---------------------------")
                    check("certyfikat sprawdzony wzgledem magazynu zaufania",
                          "certyfikat sprawdzony" in ver)
                    check("tresc pobrana po zweryfikowanym TLS",
                          "HTTP 200, tresc %d B" % len(PAGE) in ver)

                    # 4. Serwer z certyfikatem na krzywej P-256: caly
                    #    lancuch i CertificateVerify ida przez ECDSA
                    ec_port, ec_stop = start_https_server(ec=True)
                    if ec_port is not None:
                        try:
                            q.type_line("clear")
                            q.type_line("run httpget 10.0.2.2 %d /eccert.der "
                                        "roots.der" % http_port)
                            q.wait_for("tresc ", timeout=30)
                            q.type_line("clear")
                            q.type_line("run httpget https://protonos.test %d /"
                                        % ec_port)
                            ecres = q.wait_for("tresc ", timeout=90)
                            check("certyfikat na krzywej P-256 sprawdzony "
                                  "(ECDSA)",
                                  "certyfikat sprawdzony" in ecres)
                            check("tresc pobrana po TLS z kluczem EC",
                                  "HTTP 200, tresc %d B" % len(PAGE) in ecres)
                        finally:
                            ec_stop()

                        # wroc do magazynu z certyfikatem RSA
                        q.type_line("clear")
                        q.type_line("run httpget 10.0.2.2 %d /cert.der "
                                    "roots.der" % http_port)
                        q.wait_for("tresc ", timeout=30)

                    # 4b. Krzywa P-384 (podpisy SHA-384) - taka jest m.in.
                    #     ISRG Root X2
                    p384_port, p384_stop = start_https_server(ec=True,
                                                              p384=True)
                    if p384_port is not None:
                        try:
                            q.type_line("clear")
                            q.type_line("run httpget 10.0.2.2 %d "
                                        "/p384cert.der roots.der" % http_port)
                            q.wait_for("tresc ", timeout=30)
                            q.type_line("clear")
                            q.type_line("run httpget https://protonos.test %d /"
                                        % p384_port)
                            p3 = q.wait_for("tresc ", timeout=120)
                            check("certyfikat na krzywej P-384 sprawdzony "
                                  "(ECDSA + SHA-384)",
                                  "certyfikat sprawdzony" in p3)
                        finally:
                            p384_stop()

                    # 5. Lancuch dwupoziomowy: ufamy tylko korzeniowi,
                    #    wiec system musi przejsc przez posrednika
                    ch_port, ch_stop = start_https_chain_server()
                    if ch_port is not None:
                        try:
                            q.type_line("clear")
                            q.type_line("run httpget 10.0.2.2 %d /rootca.der "
                                        "roots.der" % http_port)
                            q.wait_for("tresc ", timeout=30)
                            q.type_line("clear")
                            q.type_line("run httpget https://protonos.test %d /"
                                        % ch_port)
                            chres = q.wait_for("tresc ", timeout=90)
                            check("lancuch przez posrednika domkniety do "
                                  "zaufanego korzenia",
                                  "certyfikat sprawdzony" in chres)
                        finally:
                            ch_stop()

                        q.type_line("clear")
                        q.type_line("run httpget 10.0.2.2 %d /cert.der "
                                    "roots.der" % http_port)
                        q.wait_for("tresc ", timeout=30)

                    # 6. Ten sam serwer pod adresem IP - nazwa sie nie zgadza,
                    #    wiec polaczenie ma zostac zerwane
                    q.type_line("clear")
                    q.type_line("run httpget https://10.0.2.2 %d /" % https_port)
                    bad = q.wait_for("nie udalo sie polaczyc", timeout=60)
                    check("zla nazwa w certyfikacie odrzucona",
                          "certyfikat nie obejmuje tej nazwy" in bad)
                finally:
                    https_stop()
                    dns_stop()

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
        widok = q.wait_for("ush:/")  # prompt pokazuje katalog roboczy (root = /)
        check("ush wystartowal w ring 3", "ush:/" in widok)

        q.type_line("pid")
        check("ush zna swoj pid (sys_getpid)",
              "moj pid to" in q.wait_for("moj pid to"))

        q.type_line("hello")
        widok = q.wait_for("[hello] koncze sie grzecznie")
        check("ush odpalil hello przez spawn+wait",
              "[hello] koncze sie grzecznie" in widok)

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
        widok = q.wait_for("ush: pa!")
        check("ush wyszedl przez sys_exit", "ush: pa!" in widok)

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

    return wynik()


if __name__ == "__main__":
    sys.exit(main())
