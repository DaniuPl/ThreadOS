#!/usr/bin/env python3
"""Test dymny ProtonOS: karta sieciowa e1000 (Intel 82540EM).

Do tej pory system mial jedna karte i widac to bylo w kazdej linii,
ktora potrzebowala adresu MAC. Teraz nad sterownikami stoi warstwa
netdev - to samo, co blk.c robi dla dyskow - a ktory sterownik
odpowiedzial, jest sprawa rozruchu.

e1000 jest zbudowana zupelnie inaczej niz RTL8139: rejestry ma
w pamieci zamiast w portach I/O, a ruch chodzi przez dwa pierscienie
deskryptorow zamiast jednego pierscienia bajtow. Dla ARP-a i IP nie
ma to zadnego znaczenia i wlasnie to sprawdzamy: ten sam kod sieciowy,
ta sama droga pakietu, inna karta pod spodem.

Sprawdzamy obie strony ruchu na obu jadrach - odbior bez odbioru nie
znaczy nic, a nadawanie bez odpowiedzi nie dowodzi niczego poza tym,
ze cos wyszlo w kabel.
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
from ptest import check, summary, make_iso64, no64


def main():
    # --- Jadro 32-bitowe ---
    print("--- karta e1000 (jadro 32-bitowe) ---")
    slog = tempfile.mktemp(suffix=".log")
    q = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog, nic="e1000")
    try:
        start = q.wait_for("Siec:", timeout=40)
        check("jadro znajduje karte e1000 i mowi, ktora to",
              re.search(r"Siec: e1000 ([0-9a-f]+:){5}", start) is not None)

        # DHCP to pelna droga w obie strony: pytanie w rozglaszaniu,
        # odpowiedz serwera, przyjecie dzierzawy
        lease = q.wait_for("[siec] DHCP", timeout=30)
        time.sleep(0.5)
        lease = q.screen()
        check("DHCP przez e1000 przyznal adres",
              "przyznano 10.0.2.15" in lease)

        q.type_line("ifconfig")
        ifc = q.wait_for("eth0  e1000", timeout=20)
        check("ifconfig nazywa karte po imieniu",
              "eth0  e1000, MAC" in ifc)

        # ICMP: brama slirpa odpowiada, wiec ramka naprawde wyszla
        # i naprawde wrocila
        q.type_line("clear")
        q.type_line("ping 10.0.2.2")
        pong = q.wait_for("40 B od 10.0.2.2", timeout=30)
        check("ping przez e1000: odpowiedz ICMP echo od bramy",
              "40 B od 10.0.2.2" in pong)
        stats = q.wait_for("--- statystyki:", timeout=30)
        check("ping przez e1000: wszystkie pakiety wrocily",
              "wyslano 4, odebrano 4" in stats)

        q.type_line("arp")
        arp = q.wait_for("10.0.2.2", timeout=20)
        check("tablica ARP zna adres sprzetowy bramy",
              re.search(r"10\.0\.2\.2\s+([0-9a-f]+:){5}", arp) is not None)

        # TCP na hosta: najdluzsza droga, jaka ta karta ma do przebycia
        http_port, http_stop = start_http_server()
        try:
            q.type_line("clear")
            q.type_line("run httpget 10.0.2.2 %d /" % http_port)
            page = q.wait_for("tresc ", timeout=40)
            check("TCP przez e1000: odpowiedz HTTP 200 z pelna trescia",
                  "[httpget] HTTP 200" in page and
                  "ProtonOS TCP dziala" in page)
        finally:
            http_stop()

        q.type_line("ifconfig")
        count = q.wait_for("RX ", timeout=20)
        m = re.search(r"RX (\d+) pakietow, TX (\d+) pakietow", count)
        check("licznikami widac ruch w obie strony",
              m is not None and int(m.group(1)) > 10 and int(m.group(2)) > 10)
    finally:
        q.close()
        if os.path.exists(slog):
            os.unlink(slog)

    # --- Jadro 64-bitowe ---
    # Rejestry karty leza wysoko i trzeba je dokleic do mapowania,
    # a tablice stron buduje w kazdym z jader inny kod - to jedyna
    # rzecz w tym sterowniku zalezna od trybu procesora
    print("--- karta e1000 (jadro 64-bitowe) ---")
    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog64 = tempfile.mktemp(suffix=".log")
    try:
        check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        http_port, http_stop = start_http_server()
        q64 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog64, nic="e1000",
                   qemu="qemu-system-x86_64")
        try:
            start = q64.wait_serial("Siec:", timeout=90)
            check("jadro 64-bitowe tez znajduje e1000",
                  re.search(r"Siec: e1000 ([0-9a-f]+:){5}", start) is not None)

            lease = q64.wait_serial("DHCP: przyznano", timeout=40)
            check("DHCP przez e1000 w trybie dlugim",
                  "przyznano 10.0.2.15" in lease)

            q64.wait_serial("proton64>", timeout=30)
            pong = q64.command("ping 10.0.2.2", "--- statystyki:",
                                 timeout=40)
            check("ping z jadra 64-bitowego przez e1000",
                  "wyslano 3, odebrano 3" in pong or
                  "wyslano 4, odebrano 4" in pong)

            page = q64.command("run httpget 10.0.2.2 %d /" % http_port,
                                 "kod wyjscia", timeout=60)
            check("TCP z pierscienia 3 przez e1000 w trybie dlugim",
                  "[httpget] HTTP 200" in page and
                  "ProtonOS TCP dziala" in page)
        finally:
            q64.close()
            http_stop()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog64):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
