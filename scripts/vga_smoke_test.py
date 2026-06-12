#!/usr/bin/env python3
"""Test dymny ProtonOS: bootuje jądro w QEMU bez okna i weryfikuje:
  1. boot + komunikaty inicjalizacji (Multiboot, GDT, IDT, PIC, PIT, klawiatura)
  2. obsługę wyjątku breakpoint (int3) przez IDT
  3. inicjalizację pamięci: PMM, stronicowanie, kmalloc selftest
  4. tykanie timera PIT (licznik uptime w rogu ekranu rośnie)
  5. wielozadaniowość: liczniki wątków A/B rosną niezależnie, wątek C
     kończy się i znika z listy 'ps'
  6. shell: echo klawiatury, komendy 'help', 'mem', 'ps' (QMP sendkey)
  7. tryb użytkownika: proces ring 3 robi syscalle (write/sleep), po czym
     próbuje czytać pamięć jądra - musi zginąć, a system musi żyć dalej
  8. page fault w ring 0: komenda 'crash' zatrzymuje system z diagnostyką

Użycie: python3 scripts/vga_smoke_test.py [build/protonos.bin]
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
VGA_ADDR = 0xB8000
VGA_BYTES = 80 * 25 * 2


class Qemu:
    def __init__(self, kernel):
        self.qmp_path = tempfile.mktemp(suffix=".qmp")
        self.dump_path = tempfile.mktemp(suffix=".vga")
        self.proc = subprocess.Popen(
            ["qemu-system-i386", "-kernel", kernel,
             "-display", "none", "-no-reboot",
             "-qmp", f"unix:{self.qmp_path},server,nowait"],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        time.sleep(1)
        if self.proc.poll() is not None:
            raise RuntimeError("QEMU zakonczyl sie przedwczesnie:\n"
                               + self.proc.stderr.read().decode())
        sock = socket.socket(socket.AF_UNIX)
        sock.connect(self.qmp_path)
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

    KEYNAMES = {" ": "spc", "\n": "ret", "-": "minus", ".": "dot",
                "/": "slash"}

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

    q = Qemu(KERNEL)
    try:
        # Czekaj na pelny boot (prompt shella) zamiast zgadywac czas
        screen = q.wait_for("ProtonOS shell")
        print("=== Ekran VGA po boocie ===")
        print(screen)
        print("===========================")

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
              "nieznana komenda: 'proton 1X'" in screen)

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

        # Tryb uzytkownika: proces ring 3 + syscalle + ochrona pamieci
        q.type_line("user")
        screen = q.wait_for("[ring3] czesc z trybu uzytkownika!")
        check("proces ring 3 wystartowal i robi syscall write", True)
        screen = q.wait_for("[ring3] licze: 3")
        check("syscalle write+sleep dzialaja w petli", True)
        screen = q.wait_for("proces 'user-demo' ubity")
        check("proba czytania pamieci jadra ubila proces", True)
        check("CR2 pokazuje atakowany adres", "siegnal po adres 100000" in screen)
        check("kod po ataku nie wykonal sie",
              "TEGO NIE POWINNO BYC WIDAC" not in screen)

        # System musi zyc dalej: liczniki rosna, shell odpowiada
        a3, _ = counters(q.screen())
        time.sleep(1)
        a4, _ = counters(q.screen())
        check(f"system zyje po ubiciu procesu ({a3} -> {a4})",
              a3 is not None and a4 is not None and a4 > a3)
        q.type_line("uptime")
        check("shell odpowiada po ubiciu procesu",
              "tyknac timera" in q.screen())

        # Na koncu: crash powinien wywolac page fault z diagnostyka
        q.type_line("crash")
        screen = q.screen()
        check("page fault obsluzony (wyjatek 14)",
              "WYJATEK CPU 14: Page Fault" in screen)
        check("CR2 wskazuje felerny adres",
              "(CR2): e0000000" in screen)
        check("system zatrzymany po wyjatku", "System zatrzymany" in screen)

        print()
        if failures:
            print(f"NIEPOWODZENIE: {len(failures)} test(y) nie przeszly")
            return 1
        print("WSZYSTKIE TESTY PRZESZLY")
        return 0
    finally:
        q.close()


if __name__ == "__main__":
    sys.exit(main())
