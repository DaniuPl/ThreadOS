#!/usr/bin/env python3
"""Test dymny ProtonOS: bootuje jądro w QEMU bez okna i weryfikuje:
  1. boot + komunikaty inicjalizacji (Multiboot, GDT, IDT, PIC, PIT, klawiatura)
  2. obsługę wyjątku breakpoint (int3) przez IDT
  3. tykanie timera PIT (licznik uptime w rogu ekranu rośnie)
  4. echo klawiatury PS/2 (klawisze wstrzykiwane przez QMP sendkey)

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
        time.sleep(2)
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

    def sendkey(self, key):
        self.hmp(f"sendkey {key}")
        time.sleep(0.1)

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
        screen = q.screen()
        print("=== Ekran VGA po boocie ===")
        print(screen)
        print("===========================")

        for expected in ["Multiboot OK", "GDT zaladowana", "IDT zaladowana",
                         "PIC przemapowany", "Timer PIT", "Klawiatura PS/2",
                         "Przerwania wlaczone"]:
            check(f"komunikat '{expected}'", expected in screen)

        check("obsluga wyjatku int3 (breakpoint)", "[INT3]" in screen)
        check("jadro przezylo int3 (doszlo do 'System gotowy')",
              "System gotowy" in screen)

        # Timer: uptime w rogu ekranu musi urosnac
        def uptime(s):
            m = re.search(r"uptime: (\d+)s", s)
            return int(m.group(1)) if m else None

        t1 = uptime(screen)
        time.sleep(2)
        t2 = uptime(q.screen())
        check(f"timer PIT tyka (uptime {t1}s -> {t2}s)",
              t1 is not None and t2 is not None and t2 > t1)

        # Klawiatura: wstrzyknij "proton 1" i sprawdz echo
        for key in ["p", "r", "o", "t", "o", "n", "spc", "1"]:
            q.sendkey(key)
        q.sendkey("shift-x")  # test shifta: powinno wyjsc 'X'
        screen = q.screen()
        check("echo klawiatury: 'proton 1'", "> proton 1" in screen)
        check("echo klawiatury z shiftem: 'X'", "> proton 1X" in screen)

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
