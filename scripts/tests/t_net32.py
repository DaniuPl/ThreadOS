#!/usr/bin/env python3
"""ProtonOS smoke test: the network in the 32-bit kernel (ARP, ICMP,
UDP, DNS).

Everything here talks to slirp's gateway (10.0.2.2) or to a server the
test itself puts up on the host, so none of the checks depends on
whether the machine this runs on has internet.

One assertion is not about the network at all and is here on purpose:
'ping' sleeps in the shell's thread for four seconds, and a command
typed during that time is to wait in the keyboard buffer and run right
after it. In the old days the shell ran in interrupt context, so it
could not sleep.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, start_dns_server, DNS_HINT


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        q.wait_for("ProtonOS shell", timeout=40)
        # The IP address comes from slirp's DHCP server only after the
        # shell has started - without waiting for it every network test
        # would be a race with the lease
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
        q.type_line("dns 10.0.2.3")     # typing ahead during the ping
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

        # UDP sockets in ring 3: two processes talk through the loopback,
        # so the test does not depend on any network outside
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

        # A socket has an owner: after the process dies the port has to be
        # freed
        q.type_line("kill udpecho")
        time.sleep(0.5)
        q.type_line("clear")          # a clear screen: the old "nasluchuje" disappears
        q.type_line("run udpecho")
        again = q.wait_for("porcie 7777", timeout=10)
        check("port zwolniony po smierci wlasciciela gniazda",
              "nasluchuje na porcie 7777" in again
              and "nie udalo sie otworzyc gniazda" not in again)
        q.type_line("kill udpecho")

        # DNS from ring 3. We ask our own server standing on the host
        # (10.0.2.2), so the test depends neither on the internet nor on
        # the machine's resolver.
        dns_stop, dns_ok = start_dns_server()
        if not dns_ok:
            print("POMINIETE (DNS): " + DNS_HINT)
        else:
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
        # The output of the previous 'ifconfig' (from before the ping)
        # hangs on the screen too - what counts is the last, freshest
        # occurrence
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
