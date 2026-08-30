#!/usr/bin/env python3
"""Uruchamia grupy testu dymnego ProtonOS - domyslnie rownolegle.

Kazda grupa potrzebuje wlasnej maszyny QEMU, a te i tak wiekszosc czasu
czekaja: na rozruch, na odpowiedz, na przerysowanie ekranu. Puszczanie
ich jedna po drugiej zajmowalo kilkanascie minut, z czego procesor byl
zajety przez ulamek tego czasu.

Wyjscie kazdej grupy trzymamy w calosci i wypisujemy dopiero po jej
zakonczeniu - inaczej wiersze z kilku maszyn wymieszalyby sie w jedna
nieczytelna kolumne.

  make test              wszystkie grupy
  make test T=k64        tylko jadro 64-bitowe
  make test J=1          jedna po drugiej (do debugowania)
"""

import os
import subprocess
import sys
import time

KATALOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "testy")

# Kolejnosc ma znaczenie tylko przy J=1; rownolegle i tak startuja
# najdluzsze pierwsze, zeby nie zostawaly na koniec same
GRUPY = ["rdzen", "rdzen2", "tls32", "k64", "pulpit1", "pulpit2", "sterowniki",
         "siec", "siec64", "gfx64", "pulpit64", "gui64", "acpi64",
         "konsole64", "usbkbd64", "xhci", "swap64", "dysk", "shm64", "tls64", "mmap", "mmapzap", "dynlink", "e1000", "nvme", "smp", "zamki",
         "iso", "uefi"]


def main():
    argv = sys.argv[1:]
    kernele = [a for a in argv if not a.startswith("-")]
    rownolegle = 4
    wybrane = GRUPY

    for a in argv:
        if a.startswith("-j"):
            rownolegle = max(1, int(a[2:] or 4))
        elif a.startswith("-t"):
            wybrane = [g for g in a[2:].split(",") if g]

    nieznane = [g for g in wybrane if g not in GRUPY]
    if nieznane:
        print(f"Nieznane grupy: {', '.join(nieznane)}")
        print(f"Dostepne: {', '.join(GRUPY)}")
        return 2

    start = time.time()
    czeka = list(wybrane)
    biegna = []          # (nazwa, proces, czas startu)
    wyniki = []          # (nazwa, kod, sekundy, tekst)

    while czeka or biegna:
        while czeka and len(biegna) < rownolegle:
            g = czeka.pop(0)
            p = subprocess.Popen(
                [sys.executable, "-u", os.path.join(KATALOG, f"t_{g}.py")]
                + kernele,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            biegna.append((g, p, time.time()))
            print(f">>> start: {g}", flush=True)

        for wpis in list(biegna):
            g, p, t0 = wpis
            if p.poll() is None:
                continue
            biegna.remove(wpis)
            tekst = p.stdout.read().decode(errors="replace")
            wyniki.append((g, p.returncode, time.time() - t0, tekst))
            print(f"\n=== {g} ({time.time() - t0:.0f} s) "
                  f"{'OK' if p.returncode == 0 else 'BLEDY'} ===")
            print(tekst.rstrip(), flush=True)
        if biegna:
            time.sleep(0.5)

    ok = sum(t.count("\nOK:   ") + t.startswith("OK:   ")
             for _, _, _, t in wyniki)
    zle = sum(t.count("\nBLAD: ") + t.startswith("BLAD: ")
              for _, _, _, t in wyniki)

    print("\n" + "=" * 60)
    for g, kod, sek, _ in sorted(wyniki, key=lambda w: -w[2]):
        print(f"  {g:12} {sek:5.0f} s   {'OK' if kod == 0 else 'BLEDY'}")
    print(f"\nAsercji: {ok} przeszlo, {zle} nie przeszlo "
          f"(lacznie {time.time() - start:.0f} s)")

    if zle or any(kod for _, kod, _, _ in wyniki):
        print("NIEPOWODZENIE")
        return 1
    print("WSZYSTKIE TESTY PRZESZLY")
    return 0


if __name__ == "__main__":
    sys.exit(main())
