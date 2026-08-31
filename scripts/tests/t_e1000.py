#!/usr/bin/env python3
"""ProtonOS smoke test: the e1000 network card (Intel 82540EM).

Until now the system had one card and that could be seen in every line
that needed a MAC address. Now above the drivers stands the netdev layer
- the same thing blk.c does for disks - and which driver answered is a
matter of the boot.

The e1000 is built completely differently from the RTL8139: its
registers are in memory instead of in I/O ports, and the traffic goes
through two descriptor rings instead of one ring of bytes. For ARP and
IP that means nothing at all, and that is exactly what we check: the
same network code, the same path for a packet, a different card
underneath.

We check both directions of traffic on both kernels - receiving with no
reception means nothing, and sending with no answer proves nothing
beyond that something went out on the wire.
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
    # --- The 32-bit kernel ---
    print("--- karta e1000 (jadro 32-bitowe) ---")
    slog = tempfile.mktemp(suffix=".log")
    q = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog, nic="e1000")
    try:
        start = q.wait_for("Siec:", timeout=40)
        check("jadro znajduje karte e1000 i mowi, ktora to",
              re.search(r"Siec: e1000 ([0-9a-f]+:){5}", start) is not None)

        # DHCP is a full round trip: a question by broadcast, the
        # server's answer, accepting the lease
        lease = q.wait_for("[siec] DHCP", timeout=30)
        time.sleep(0.5)
        lease = q.screen()
        check("DHCP przez e1000 przyznal adres",
              "przyznano 10.0.2.15" in lease)

        q.type_line("ifconfig")
        ifc = q.wait_for("eth0  e1000", timeout=20)
        check("ifconfig nazywa karte po imieniu",
              "eth0  e1000, MAC" in ifc)

        # ICMP: slirp's gateway answers, so the frame really went out
        # and really came back
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

        # TCP to the host: the longest path this card has to travel
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

    # --- The 64-bit kernel ---
    # The card's registers lie high up and have to be added to the
    # mapping, and the page tables are built by different code in each
    # kernel - that is the only thing in this driver that depends on the
    # processor mode
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
