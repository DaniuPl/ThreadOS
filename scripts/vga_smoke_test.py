#!/usr/bin/env python3
"""Test dymny ProtonOS: bootuje jądro w QEMU bez okna i weryfikuje:
  1. boot + komunikaty inicjalizacji (Multiboot, GDT, IDT, PIC, PIT, klawiatura)
  2. obsługę wyjątku breakpoint (int3) przez IDT
  3. inicjalizację pamięci: PMM, stronicowanie, kmalloc selftest
  4. tykanie timera PIT (licznik uptime w rogu ekranu rośnie)
  5. wielozadaniowość: liczniki wątków A/B rosną niezależnie, wątek C
     kończy się i znika z listy 'ps'
  6. shell: echo klawiatury, komendy 'help', 'mem', 'ps' (QMP sendkey)
  7. procesy: 'run hello' (ELF z initrd, syscalle, czyste wyjście,
     brak wycieku ramek), 'run evil' (czyta pamięć jądra - ginie,
     system żyje), 'run brak' (błąd), 'ls'
  8. shell użytkownika (ring 3): 'run sh' przejmuje klawiaturę przez
     sys_read, spawn+wait programów, exit oddaje konsolę jądru
  9. page fault w ring 0: komenda 'crash' zatrzymuje system z diagnostyką

Użycie: python3 scripts/vga_smoke_test.py [protonos.bin] [initrd.img]
Kod wyjścia 0 = wszystko OK, 1 = błąd.
"""

import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

KERNEL = sys.argv[1] if len(sys.argv) > 1 else "build/protonos.bin"
INITRD = sys.argv[2] if len(sys.argv) > 2 else None
VGA_ADDR = 0xB8000
VGA_BYTES = 80 * 25 * 2
VIDEO_W = 320
VIDEO_H = 200


class Qemu:
    def __init__(self, kernel, initrd=None, disk=None):
        self.qmp_path = tempfile.mktemp(suffix=".qmp")
        self.dump_path = tempfile.mktemp(suffix=".vga")
        cmd = ["qemu-system-i386", "-kernel", kernel,
               "-display", "none", "-no-reboot",
               # Stala data RTC -> deterministyczny test komendy 'date'
               "-rtc", "base=2026-06-13T12:34:56",
               "-qmp", f"unix:{self.qmp_path},server,nowait"]
        if initrd:
            cmd += ["-initrd", initrd]
        if disk:
            cmd += ["-hda", disk]
        self.proc = subprocess.Popen(
            cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

        # Poczekaj, az QEMU wystawi socket QMP (start bywa wolny)
        sock = socket.socket(socket.AF_UNIX)
        deadline = time.time() + 10
        while True:
            if self.proc.poll() is not None:
                raise RuntimeError("QEMU zakonczyl sie przedwczesnie:\n"
                                   + self.proc.stderr.read().decode())
            try:
                sock.connect(self.qmp_path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > deadline:
                    raise
                time.sleep(0.2)
        self.f = sock.makefile("rwb")
        self.f.readline()  # baner QMP
        self.cmd({"execute": "qmp_capabilities"})

    def cmd(self, msg):
        self.f.write((json.dumps(msg) + "\n").encode())
        self.f.flush()
        while True:
            resp = json.loads(self.f.readline())
            if "return" in resp or "error" in resp:
                if "error" in resp:
                    raise RuntimeError(f"Blad QMP: {resp}")
                return resp

    def hmp(self, command):
        return self.cmd({"execute": "human-monitor-command",
                         "arguments": {"command-line": command}})

    def screen(self):
        self.hmp(f'pmemsave {VGA_ADDR} {VGA_BYTES} "{self.dump_path}"')
        raw = open(self.dump_path, "rb").read()
        lines = []
        for y in range(25):
            chars = raw[y * 160:(y + 1) * 160:2]
            lines.append(chars.decode("cp437", "replace").rstrip())
        return "\n".join(lines).rstrip()

    def wait_for(self, text, timeout=15):
        """Czeka, az podany tekst pojawi sie na ekranie VGA."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            screen = self.screen()
            if text in screen:
                return screen
            time.sleep(0.3)
        raise RuntimeError(f"timeout: '{text}' nie pojawilo sie na ekranie")

    def sendkey(self, key):
        self.hmp(f"sendkey {key}")
        time.sleep(0.1)

    def mouse_move(self, dx, dy):
        self.cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "rel", "data": {"axis": "x", "value": dx}},
            {"type": "rel", "data": {"axis": "y", "value": dy}}]}})
        time.sleep(0.2)

    def mouse_button(self, down):
        self.cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"button": "left", "down": down}}]}})
        time.sleep(0.2)

    # Sledzenie pozycji kursora (pulpit startuje na srodku 320x200)
    mx, my = 160, 100

    def mouse_goto(self, tx, ty):
        self.mouse_move(tx - self.mx, ty - self.my)
        self.mx, self.my = tx, ty

    def mouse_click(self):
        self.mouse_button(True)
        self.mouse_button(False)

    def screen_pixels(self):
        """Zrzut ekranu (QMP screendump) -> (skala_x, skala_y, funkcja
        pikseli we wspolrzednych framebuffera 320x200). QEMU podwaja
        tryb 13h przy wyswietlaniu, stad skalowanie."""
        ppm = tempfile.mktemp(suffix=".ppm")
        self.cmd({"execute": "screendump", "arguments": {"filename": ppm}})
        data = open(ppm, "rb").read()
        os.unlink(ppm)
        idx, fields = 2, []
        while len(fields) < 3:
            while data[idx] in b" \t\n\r":
                idx += 1
            s = idx
            while data[idx] not in b" \t\n\r":
                idx += 1
            fields.append(int(data[s:idx]))
        idx += 1
        w, h, _ = fields
        px = data[idx:]
        sx, sy = w // VIDEO_W, h // VIDEO_H

        def at(fx, fy):
            x, y = fx * sx + 1, fy * sy + 1
            o = (y * w + x) * 3
            return (px[o], px[o + 1], px[o + 2])
        return sx, sy, at

    def wait_pixel(self, x, y, want, tol=40, timeout=4):
        """Czeka, az piksel (x,y) osiagnie kolor 'want' (z tolerancja).
        Niweluje flakiness: kompozytor hi-res odswieza wolniej niz
        przychodza zdarzenia. Zwraca True/False."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            _, _, at = self.screen_raw()
            p = at(x, y)
            if all(abs(p[i] - want[i]) <= tol for i in range(3)):
                return True
            time.sleep(0.2)
        return False

    def screen_raw(self):
        """Zrzut ekranu w natywnych wspolrzednych (bez skalowania) -
        (szerokosc, wysokosc, funkcja pikseli). Do trybow VESA."""
        ppm = tempfile.mktemp(suffix=".ppm")
        self.cmd({"execute": "screendump", "arguments": {"filename": ppm}})
        data = open(ppm, "rb").read()
        os.unlink(ppm)
        idx, fields = 2, []
        while len(fields) < 3:
            while data[idx] in b" \t\n\r":
                idx += 1
            s = idx
            while data[idx] not in b" \t\n\r":
                idx += 1
            fields.append(int(data[s:idx]))
        idx += 1
        w, h, _ = fields
        px = data[idx:]

        def at(x, y):
            o = (y * w + x) * 3
            return (px[o], px[o + 1], px[o + 2])
        return w, h, at

    KEYNAMES = {" ": "spc", "\n": "ret", "-": "minus", ".": "dot",
                "/": "slash", ">": "shift-dot", "<": "shift-comma",
                "|": "shift-backslash", "&": "shift-7"}

    def type_line(self, text):
        for ch in text:
            self.sendkey(self.KEYNAMES.get(ch, ch))
        self.sendkey("ret")
        time.sleep(0.3)

    def close(self):
        self.proc.kill()
        for p in (self.qmp_path, self.dump_path):
            if os.path.exists(p):
                os.unlink(p)


def main():
    failures = []

    def check(name, cond):
        print(("OK:   " if cond else "BLAD: ") + name)
        if not cond:
            failures.append(name)

    # Swiezy obraz dysku (1 MiB) wspoldzielony przez oba boot-y -
    # jadro sformatuje go przy pierwszym starcie
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * (1024 * 1024))

    q = Qemu(KERNEL, INITRD, disk)
    try:
        # Czekaj na pelny boot (prompt shella) zamiast zgadywac czas
        screen = q.wait_for("ProtonOS shell")
        print("=== Ekran VGA po boocie ===")
        print(screen)
        print("===========================")

        check("dysk ATA + PFS wykryty i sformatowany",
              "Dysk ATA + PFS gotowy" in screen)
        check("mysz PS/2 wykryta przy boocie",
              "Mysz PS/2 gotowa" in screen)

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
        check("watek C zakonczyl prace", True)

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

        q.type_line("uptime")
        screen = q.screen()
        check("komenda 'uptime' dziala", "tyknac timera" in screen)

        q.type_line("ps")
        screen = q.screen()
        check("komenda 'ps' pokazuje watki",
              "main (biezacy)" in screen and "demo-A" in screen
              and "demo-B" in screen)
        check("zombie po watku C zostal sprzatniety",
              "demo-C" not in screen)

        # initrd: 'ls' pokazuje spakowane programy
        q.type_line("ls")
        screen = q.screen()
        check("komenda 'ls' pokazuje pliki initrd",
              "hello" in screen and "evil" in screen)

        # Wyciek ramek: zmierz uzycie PMM przed i po zyciu procesu
        def pmm_used(s):
            m = re.findall(r"PMM:\s+(\d+)/", s)
            return int(m[-1]) if m else None

        q.type_line("mem")
        used_before = pmm_used(q.screen())

        # 'run hello': proces ELF z wlasna przestrzenia adresowa
        q.type_line("run hello")
        q.wait_for("[hello] czesc, jestem procesem ELF")
        check("proces hello wystartowal z ELF i robi syscall write", True)
        q.wait_for("[hello] licze: 3")
        check("syscalle write+sleep dzialaja w petli", True)
        q.wait_for("proces 'hello' zakonczyl sie (kod 0)")
        check("hello zakonczyl sie czysto przez sys_exit", True)

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

        # 'run evil': proba czytania pamieci jadra
        q.type_line("run evil")
        screen = q.wait_for("proces 'evil' ubity")
        check("evil czytal pamiec jadra i zostal ubity", True)
        check("CR2 pokazuje atakowany adres", "siegnal po adres 100000" in screen)
        check("kod po ataku nie wykonal sie",
              "TEGO NIE POWINNO BYC WIDAC" not in screen)

        # 'run' z nieistniejacym plikiem
        q.type_line("run brak")
        check("run zgasza brak pliku", "brak pliku 'brak'" in q.screen())

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
        q.wait_for("ush:/")  # prompt pokazuje katalog roboczy (root = /)
        check("ush wystartowal w ring 3", True)

        q.type_line("pid")
        check("ush zna swoj pid (sys_getpid)", "moj pid to" in q.wait_for("moj pid to"))

        q.type_line("hello")
        q.wait_for("[hello] koncze sie grzecznie")
        check("ush odpalil hello przez spawn+wait", True)

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
        screen = q.wait_for("2026-06-13")
        m = re.search(r"2026-06-13 (\d\d):(\d\d):(\d\d)", screen)
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
        q.wait_for("ush: pa!")
        check("ush wyszedl przez sys_exit", True)

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
    q2 = Qemu(KERNEL, INITRD, disk)
    try:
        screen = q2.wait_for("ProtonOS shell")
        check("drugi boot: dysk wykryty (juz sformatowany, bez ponownego format)",
              "Dysk ATA + PFS gotowy" in screen
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
    finally:
        q2.close()
        if os.path.exists(disk):
            os.unlink(disk)

    # --- Tryb graficzny (osobny boot: 'gfx' jest jednokierunkowy) ---
    print("--- tryb graficzny (screendump) ---")
    q3 = Qemu(KERNEL, INITRD)
    try:
        q3.wait_for("ProtonOS shell")
        q3.type_line("gfx")
        time.sleep(1.0)
        sx, sy, at = q3.screen_pixels()
        check(f"tryb graficzny aktywny (ekran skalowany {sx}x{sy})",
              sx >= 1 and sy >= 1)

        def near(px, want, tol=40):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        check("framebuffer: tlo niebieskie (fb 160,180)",
              near(at(160, 180), (0, 0, 255)))
        check("framebuffer: czerwony prostokat na srodku (fb 160,100)",
              near(at(160, 100), (255, 0, 0)))
        check("framebuffer: zielony prostokat w rogu (fb 30,30)",
              near(at(30, 30), (0, 255, 0)))
        check("framebuffer: zolty prostokat w rogu (fb 290,30)",
              near(at(290, 30), (255, 255, 0)))
    finally:
        q3.close()

    # --- Pulpit graficzny: kursor myszy (osobny boot) ---
    print("--- pulpit: kursor myszy ---")
    q4 = Qemu(KERNEL, INITRD)
    try:
        q4.wait_for("ProtonOS shell")
        q4.type_line("desktop")
        time.sleep(1.0)

        def near(px, want, tol=40):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # Pulpit dziala w wysokiej rozdzielczosci VESA (640x480)
        gw, gh, _ = q4.screen_raw()
        check(f"pulpit w trybie VESA wysokiej rozdzielczosci ({gw}x{gh})",
              gw == 640 and gh == 480)

        # mysz startuje na srodku (160,100); przesun do ~(50,40)
        q4.mouse_move(-110, -60)
        check("kursor rysowany w pozycji myszy (bialy ~50,41)",
              q4.wait_pixel(50, 41, (255, 255, 255)))
        _, _, at = q4.screen_raw()
        check("tlo pulpitu (teal) z dala od kursora i okien",
              near(at(10, 150), (0, 128, 160)))
        # przesun w inne miejsce - kursor podaza
        q4.mouse_move(150, 80)
        check("kursor podaza za mysza (bialy ~200,121)",
              q4.wait_pixel(200, 121, (255, 255, 255)))
        check("poprzednia pozycja kursora wyczyszczona (teal ~50,41)",
              q4.wait_pixel(50, 41, (0, 128, 160)))

        # Tekst (czcionka 8x8): pasek tytulu z napisem "ProtonOS".
        # Pasek jasnoszary; glif czarny -> komorka litery ma i czarne,
        # i szare piksele (czyli to ksztalt, nie pelny prostokat).
        _, _, at = q4.screen_raw()
        check("pasek tytulu narysowany (jasnoszary, puste miejsce)",
              near(at(200, 4), (192, 192, 192)))
        cell = [at(2 + c, 1 + r) for r in range(8) for c in range(8)]
        blk = sum(1 for p in cell if near(p, (0, 0, 0)))
        gry = sum(1 for p in cell if near(p, (192, 192, 192)))
        check(f"tekst renderowany czcionka 8x8 (glif 'P': {blk} czarnych, "
              f"{gry} szarych)", blk > 5 and gry > 5)
    finally:
        q4.close()

    # --- Pulpit: okna i przeciaganie mysza (osobny boot) ---
    print("--- pulpit: okna + drag ---")
    q5 = Qemu(KERNEL, INITRD)
    try:
        q5.wait_for("ProtonOS shell")
        q5.type_line("desktop")
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # Okna: win0 (40,60,120x70) i win1 (180,95). Mysz startuje (160,100).
        check("okno 0: pasek tytulu niebieski (80,64)",
              q5.wait_pixel(80, 64, (0, 0, 255)))
        _, _, at = q5.screen_raw()
        check("okno 0: tlo szare (80,100)",
              near(at(80, 100), (224, 224, 224)))
        check("okno 1: pasek tytulu niebieski (220,99)",
              near(at(220, 99), (0, 0, 255)))

        # Przeciaganie okna 0: kursor (160,100) -> pasek win0 (60,64),
        # wcisnij LPM, przesun (+80,+50), pusc. Okno -> (120,110).
        q5.mouse_move(-100, -36)
        q5.mouse_button(True)
        q5.mouse_move(80, 50)
        q5.mouse_button(False)
        # nowy pasek win0 w widocznej (niezaslonietej) czesci: (150,114)
        check("okno 0 przeciagniete na nowa pozycje (pasek ~150,114)",
              q5.wait_pixel(150, 114, (0, 0, 255)))
        _, _, at = q5.screen_raw()
        check("stare miejsce okna 0 wyczyszczone (teal ~60,90)",
              near(at(60, 90), (0, 128, 160)))
        check("okno 1 nieruszone podczas przeciagania (220,99)",
              near(at(220, 99), (0, 0, 255)))
    finally:
        q5.close()

    # --- Pulpit: klikalne kontrolki (przycisk akcji + zamkniecie) ---
    print("--- pulpit: kontrolki (klik) ---")
    q6 = Qemu(KERNEL, INITRD)
    try:
        q6.wait_for("ProtonOS shell")
        q6.type_line("desktop")
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # win0 (40,60,120x70): przycisk [x] czerwony w prawym rogu paska
        check("przycisk zamkniecia [x] (czerwony, ~153,63)",
              q6.wait_pixel(153, 63, (255, 0, 0)))
        _, _, at = q6.screen_raw()

        # Licznik przy przycisku 'Klik' (komorka ~94,80). Klikamy 3x.
        cell_before = [at(94 + c, 80 + r) for r in range(8) for c in range(8)]
        q6.mouse_goto(68, 84)            # srodek przycisku 'Klik'
        for _ in range(3):
            q6.mouse_click()
        time.sleep(0.6)
        _, _, at = q6.screen_raw()
        cell_after = [at(94 + c, 80 + r) for r in range(8) for c in range(8)]
        white_after = sum(1 for p in cell_after if near(p, (255, 255, 255), 10))
        check("przycisk akcji reaguje na klik (licznik zmienil cyfre)",
              cell_before != cell_after and white_after > 0)

        # Zamkniecie okna: klik [x] okna 1 (close box ~293,99)
        q6.mouse_goto(293, 99)
        q6.mouse_click()
        check("klik [x] zamyka okno (pasek win1 znika, wraca tlo)",
              q6.wait_pixel(220, 99, (0, 128, 160)))
        _, _, at = q6.screen_raw()
        # okno 0 nadal jest (klik nie ruszyl innych okien)
        check("inne okno pozostaje otwarte po zamknieciu (win0 pasek 80,64)",
              near(at(80, 64), (0, 0, 255)))
    finally:
        q6.close()

    # --- Aplikacja ring 3 z wlasnym oknem (rysowanie przez syscalle) ---
    print("--- pulpit: aplikacja ring 3 ---")
    q7 = Qemu(KERNEL, INITRD)
    try:
        q7.wait_for("ProtonOS shell")
        q7.type_line("desktop")
        time.sleep(0.8)
        q7.type_line("run guiapp")
        time.sleep(1.2)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # guiapp -> slot 2 (sloty 0/1 to okna jadra): okno (94,54),
        # obszar treści od (95,65). Kwadraty RGB rysowane przez syscalle.
        check("aplikacja ring3 narysowala czerwony kwadrat (101,71)",
              q7.wait_pixel(101, 71, (255, 0, 0)))
        check("aplikacja ring3 narysowala niebieski kwadrat (170,75)",
              q7.wait_pixel(170, 75, (0, 0, 255)))

        # Zdarzenia: klik w cialo okna trafia do aplikacji (win_poll),
        # ktora rysuje bialy znacznik w miejscu klikniecia. Licznik
        # 'klikniec:' przy (175,89) rosnie. Kursor odsuwamy, by nie
        # zaslonil znacznika przy zrzucie.
        cnt_before = [at(175 + c, 89 + r) for r in range(8) for c in range(8)]
        q7.mouse_goto(150, 110)
        q7.mouse_click()
        q7.mouse_goto(60, 140)            # odsun kursor od znacznika
        check("klik dostarczony do aplikacji: znacznik w miejscu (149,109)",
              q7.wait_pixel(149, 109, (255, 255, 255)))
        _, _, at = q7.screen_raw()
        cnt_after = [at(175 + c, 89 + r) for r in range(8) for c in range(8)]
        check("aplikacja zliczyla klikniecie (licznik zmienil cyfre)",
              cnt_before != cnt_after)

        # Klawiatura do okna z fokusem: okno jest sfokusowane (klikniete
        # wyzej). Po wpisaniu znakow pole 'txt:' (~135,103) pokazuje tekst.
        for k in ("a", "b", "c"):
            q7.sendkey(k)

        def txt_white():
            _, _, a = q7.screen_raw()
            return sum(1 for x in range(135, 159) for y in range(103, 111)
                       if near(a(x, y), (255, 255, 255), 10))

        appeared = False
        for _ in range(15):          # poll: tekst pojawia sie po kilku klatkach
            if txt_white() > 0:
                appeared = True
                break
            time.sleep(0.2)
        check("klawiatura trafia do okna z fokusem (tekst pojawil sie)",
              appeared)

        # Klik w pulpit (puste tlo) zdejmuje fokus, by 'kill 5' trafilo
        # do shella jadra, a nie do okna aplikacji
        q7.mouse_goto(60, 140)
        q7.mouse_click()
        time.sleep(0.2)

        # Zabicie aplikacji: kompozytor sprzata jej okno (pid 5)
        q7.type_line("kill 5")
        check("po zabiciu aplikacji jej okno zostaje sprzatniete (170,75 teal)",
              q7.wait_pixel(170, 75, (0, 128, 160)))
    finally:
        q7.close()

    # --- Pasek zadan i menu uruchamiania (osobny boot) ---
    print("--- pulpit: pasek zadan + menu ---")
    q8 = Qemu(KERNEL, INITRD)
    try:
        q8.wait_for("ProtonOS shell")
        q8.type_line("desktop")

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # Pasek zadan na dole (640x480 -> y=468); przycisk 'Menu' x2..42
        check("pasek zadan: przycisk 'Menu' (niebieski, 30,470)",
              q8.wait_pixel(30, 470, (0, 0, 255)))
        _, _, at = q8.screen_raw()
        # przed uruchomieniem aplikacji slot 2 jest pusty (tlo okna jadra)
        check("przed launchem brak okna aplikacji (101,71 nie czerwone)",
              not near(at(101, 71), (255, 0, 0)))

        # Menu ma 3 wpisy (guiapp/gclock/gfiles): entry0 'guiapp' y432..444
        q8.mouse_goto(10, 472)
        q8.mouse_click()
        check("klik 'Menu' rozwija menu (wpis szary 70,438)",
              q8.wait_pixel(70, 438, (224, 224, 224)))

        # Klik wpisu 'guiapp' (entry0) -> uruchamia aplikacje
        q8.mouse_goto(40, 438)
        q8.mouse_click()
        check("wybor z menu uruchamia aplikacje (czerwony kwadrat 101,71)",
              q8.wait_pixel(101, 71, (255, 0, 0)))
        _, _, at = q8.screen_raw()
        check("menu zamyka sie po wyborze (70,438 nie szare menu)",
              not near(at(70, 438), (224, 224, 224)))

        # Lista okien na pasku zadan: przyciski Terminal/Pliki/guiapp.
        # guiapp jest na wierzchu (swiezo uruchomione) - czerwony przy (101,71).
        # Klik przycisku 'Terminal' (slot0, j0, x46..96, y468) wynosi je na
        # wierzch i fokusuje: czerwony guiappa zostaje zakryty oknem jadra.
        q8.mouse_goto(60, 473)
        q8.mouse_click()
        check("klik przycisku na pasku wynosi okno na wierzch (czerwony zakryty)",
              q8.wait_pixel(101, 71, (224, 224, 224), 15))
        check("fokusowane okno podswietlone na pasku zadan (niebieski 90,469)",
              q8.wait_pixel(90, 469, (0, 0, 255)))
    finally:
        q8.close()

    # --- Menedzer plikow GUI: FS -> okno aplikacji (osobny boot z dyskiem) ---
    print("--- pulpit: menedzer plikow (gfiles) ---")
    gdisk = tempfile.mktemp(suffix=".img")
    with open(gdisk, "wb") as f:
        f.write(b"\0" * (1024 * 1024))
    q9 = Qemu(KERNEL, INITRD, gdisk)
    try:
        q9.wait_for("ProtonOS shell")
        q9.type_line("write raport abc")   # plik na dysku (przed pulpitem)
        time.sleep(0.5)
        q9.type_line("desktop")
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # Menu -> wpis 'gfiles' (entry2, y456..468)
        q9.mouse_goto(10, 472)
        q9.mouse_click()
        q9.mouse_goto(40, 462)
        q9.mouse_click()
        # gfiles slot2 (94,54), content (95,65); nazwa pliku przy
        # content(8,16) -> screen(103,81). Tekst (bialy) = pliki z PFS.
        def files_row_white():
            _, _, a = q9.screen_raw()
            return sum(1 for x in range(103, 160) for y in range(81, 89)
                       if near(a(x, y), (255, 255, 255), 10))
        appeared = False
        for _ in range(15):
            if files_row_white() > 0:
                appeared = True
                break
            time.sleep(0.2)
        check("menedzer plikow GUI listuje plik z dysku (FS -> okno)",
              appeared)
    finally:
        q9.close()
        if os.path.exists(gdisk):
            os.unlink(gdisk)

    print()
    if failures:
        print(f"NIEPOWODZENIE: {len(failures)} test(y) nie przeszly")
        return 1
    print("WSZYSTKIE TESTY PRZESZLY")
    return 0


if __name__ == "__main__":
    sys.exit(main())
