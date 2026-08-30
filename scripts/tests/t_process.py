#!/usr/bin/env python3
"""Test dymny ProtonOS: procesy pierscienia 3 i ich pamiec.

Program uzytkownika to osobny plik ELF z wlasna przestrzenia adresowa.
Ta grupa przechodzi po kolei przez wszystko, co taki program dostaje od
jadra: wywolania systemowe, argumenty, rosnaca sterte, mmap, pamiec
wspoldzielona, watki i priorytety.

Po KAZDYM programie pytamy 'mem' o liczbe zajetych ramek i porownujemy
ja z ta sprzed uruchomienia. Proces, ktory sie skonczyl, ma oddac
wszystko - to jedyna asercja, ktora wylapuje wyciek pamieci, bo sam
program niczego o nim nie powie.

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
from ptest import check, summary, pmm_used


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    slog = tempfile.mktemp(suffix=".log")

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
    try:
        q.wait_for("ProtonOS shell")
        q.wait_for("[siec] DHCP", timeout=20)
        time.sleep(0.5)

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
        check("argumenty argc/argv docieraja do procesu (echo)",
              "proton os dziala" in q.wait_for("proton os dziala"))

        # 'run memtest': sbrk/malloc daje procesowi rosnaca pamiec
        q.type_line("run memtest")
        screen = q.wait_for("[memtest] OK:")
        check("sbrk/malloc dziala (memtest zapisal i zweryfikowal pamiec)",
              "blokow po" in screen and "zweryfikowanych" in screen)
        check("memtest nie zglosil bledu pamieci",
              "BLAD" not in screen.split("[memtest] OK")[0]
              .split("[memtest] start")[-1])

        time.sleep(0.5)
        q.type_line("mem")
        used_after_mt = pmm_used(q.screen())
        check(f"brak wycieku ramek po memtest"
              f" ({used_before} -> {used_after_mt})",
              used_after_mt is not None and used_after_mt == used_before)

        # 'run mmaptest': mmap daje swieza, wyzerowana, zapisywalna pamiec
        q.type_line("run mmaptest")
        check("mmap dziala (3 strony zmapowane, zerowe i zapisywalne)",
              "zapisane i zweryfikowane" in q.wait_for("[mmaptest] OK:"))
        time.sleep(0.5)
        q.type_line("mem")
        used_after_mm = pmm_used(q.screen())
        check(f"brak wycieku ramek po mmaptest"
              f" ({used_before} -> {used_after_mm})",
              used_after_mm is not None and used_after_mm == used_before)

        # 'run shmtest': pamiec wspoldzielona miedzy procesami. shmtest
        # zapisuje 12345 i uruchamia shmread (osobny proces), ktory czyta
        # te sama pamiec - dowodem jest jego linia z odczytana wartoscia.
        q.type_line("run shmtest")
        check("pamiec wspoldzielona: inny proces czyta zapisana wartosc",
              "[shmread] wartosc=12345" in q.wait_for("[shmread] wartosc="))

        # 'run threads': dwa watki w jednym procesie (wspolna pamiec).
        # Kazdy zapisuje inna zmienna globalna; main sumuje -> 33.
        # Swieza baza ramek (shmtest zajal trwaly region wspoldzielony).
        q.type_line("mem")
        used_before_th = pmm_used(q.screen())
        q.type_line("run threads")
        check("watki w procesie dziela pamiec (suma globalnych = 33)",
              "suma=33" in q.wait_for("[threads] watki zapisaly"))
        # brak wycieku ramek po procesie wielowatkowym (stosy watkow +
        # przestrzen adresowa zwolnione, gdy konczy sie ostatni watek)
        time.sleep(0.5)
        q.type_line("mem")
        used_after_th = pmm_used(q.screen())
        check(f"brak wycieku ramek po watkach"
              f" ({used_before_th} -> {used_after_th})",
              used_after_th is not None and used_after_th == used_before_th)

        # 'run prio': scheduler z priorytetami. Watek normalny wywlaszcza
        # niski -> przez to samo okno czasu naliczy wielokrotnie wiecej.
        q.type_line("run prio")
        screen = q.wait_for("[prio] normalny=", timeout=20)
        m = re.search(r"normalny=(\d+) niski=(\d+)", screen)
        check("scheduler z priorytetami: normalny >> niski"
              + (f" ({m.group(1)} vs {m.group(2)})" if m else ""),
              m is not None and int(m.group(1)) > int(m.group(2)) * 5)

        # Kryptografia pod TLS: wektory z RFC liczone w pierscieniu 3
        q.type_line("clear")
        q.type_line("run cryptotest")
        crypto = q.wait_for("[crypto] wszystkie", timeout=20)
        check("wektory SHA-256/HMAC/HKDF/ChaCha20/Poly1305/X25519 z RFC",
              "wszystkie wektory zgadzaja sie" in crypto
              and "BLAD" not in crypto)
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
