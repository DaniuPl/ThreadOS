#!/usr/bin/env python3
"""Test dymny ProtonOS: siec w jadrze 32-bitowym (ARP, ICMP, UDP, DNS).

Wszystko tutaj rozmawia z brama slirpa (10.0.2.2) albo z serwerem
postawionym przez sam test na hoscie, wiec zaden ze sprawdzianow nie
zalezy od tego, czy maszyna, na ktorej to biegnie, ma internet.

Jedna asercja nie dotyczy sieci wcale i jest tu z rozmyslem: 'ping' spi
w watku shella przez cztery sekundy, a komenda wpisana w tym czasie ma
czekac w buforze klawiatury i wykonac sie zaraz po nim. Dawniej shell
dzialal w kontekscie przerwania, wiec spac nie mogl.

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
from ptest import check, summary, start_dns_server


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        q.wait_for("ProtonOS shell", timeout=40)
        # Adres IP przychodzi z serwera DHCP slirpa juz po starcie
        # shella - bez czekania na niego kazdy test sieciowy bylby
        # wyscigiem z dzierzawa
        q.wait_for("[siec] DHCP", timeout=30)
        time.sleep(0.5)

        q.type_line("ifconfig")
        ifc = q.wait_for("eth0  RTL8139, MAC")
        check("ifconfig nazywa karte, ktora odpowiedziala",
              "eth0  RTL8139, MAC" in ifc)
        check("ifconfig pokazuje adres IP", "IP 10.0.2.15" in ifc)
        check("ifconfig pokazuje brame", "brama 10.0.2.2" in ifc)
        check("ifconfig pokazuje serwer DNS z DHCP", "DNS 10.0.2.3" in ifc)

        q.type_line("ping 10.0.2.2")
        q.type_line("dns 10.0.2.3")     # pisanie na zapas w trakcie pinga
        pong = q.wait_for("40 B od 10.0.2.2: seq=1", timeout=20)
        check("ping: odpowiedz ICMP echo od bramy",
              "40 B od 10.0.2.2" in pong)
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
              "nasluchuje na porcie 7777" in again
              and "nie udalo sie otworzyc gniazda" not in again)
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

        q.type_line("ifconfig")
        ifc = q.wait_for("RX ")
        # Na ekranie wisi tez wyjscie poprzedniego 'ifconfig' (sprzed
        # pinga) - liczy sie ostatnie, najswiezsze wystapienie
        rx = re.findall(r"RX (\d+) pakietow, TX (\d+) pakietow", ifc)
        check(f"licznik pakietow RX/TX rosnie"
              f" (ostatni: {rx[-1] if rx else None})",
              bool(rx) and int(rx[-1][0]) > 0 and int(rx[-1][1]) > 0)
    finally:
        q.close()
        if os.path.exists(disk):
            os.unlink(disk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
