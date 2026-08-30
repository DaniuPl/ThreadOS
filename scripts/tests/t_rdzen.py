#!/usr/bin/env python3
"""Test dymny ProtonOS: rozruch, shell, watki, pamiec i procesy ring 3.

Pierwsza polowa dawnej grupy 'rdzen'. Sieci, TLS-a i systemu plikow
szukaj w t_rdzen2.py. Podzial nie jest tematyczny dla ozdoby: jedna
grupa robila 174 asercje na jednej maszynie i sama decydowala, jak
dlugo trwa caly bieg, bo reszta konczyla sie duzo wczesniej.

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
    # Swiezy obraz dysku (1 MiB) wspoldzielony przez oba boot-y -
    # jadro sformatuje go przy pierwszym starcie
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)

    # Port szeregowy zbiera wszystko, co jadro wypisalo od poczatku -
    # w odroznieniu od ekranu, z ktorego starsze wiersze wyjezdzaja
    slog = tempfile.mktemp(suffix=".log")

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
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
        check("na jednym rdzeniu system mowi to wprost",
              "Procesory: 1" in q.serial())

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

        # Komunikaty rozruchowe czytamy z PORTU, nie z ekranu. Wiersz
        # zerowy nalezy do licznikow watkow demonstracyjnych, a przy
        # dluzszym rozruchu wyjezdza na niego najstarsza linia i zostaje
        # przez nie zamazana - dowod, ktory znika, gdy maszyna zwolni,
        # nie jest zadnym dowodem.
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
                 "mkmapa", "mmapfile", "gleboko", "mmapzap", "dynlib"}
        check("komenda 'ls' pokazuje pliki initrd (%d wpisow, %d znanych)"
              % (len(entries), len({n for n, _ in entries} & known)),
              len(entries) >= 15 and
              all(int(size) > 0 for _, size in entries) and
              len({n for n, _ in entries} & known) >= 3)

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
        view = q.wait_for("[hello] licze: 3")
        check("syscalle write+sleep dzialaja w petli",
              "[hello] licze: 3" in view)
        view = q.wait_for("proces 'hello' zakonczyl sie (kod 0)")
        check("hello zakonczyl sie czysto przez sys_exit",
              "proces 'hello' zakonczyl sie (kod 0)" in view)

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
        check("run zgasza brak pliku - i szuka go w obu miejscach",
              "nie ma 'brak' ani w initrd, ani na dysku" in q.screen())
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
