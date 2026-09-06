#!/usr/bin/env python3
"""ProtonOS smoke test: a network card on USB (RNDIS).

The other half of what sits in a docking station. It differs from the
two cards this system already knows in a way that goes deeper than
registers: there are no registers. Everything is a conversation -
"start yourself", "what is your address", "pass me broadcasts" -
carried as encapsulated commands through endpoint zero, and every
frame gets a forty-four byte header in front of it.

The machine here has NO card of its own: the PCI network device is
left out on purpose, so that anything that works, works through the
USB bus. And what has to work is not "the card was found" but the
whole way up: an address from DHCP, an answer to a ping, and a page
fetched over TCP from a server on the host - the same server the other
network tests use, so the same evidence.

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, make_iso64, start_http_server


def main():
    print()
    print("--- karta sieciowa na USB (RNDIS) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)

    try:
        check("plyta z jadrem 64-bitowym buduje sie",
              make_iso64(d, iso, text=True))
        if not os.path.exists(iso):
            return summary()

        port, stop = start_http_server()
        # nic=None: the machine has no card of its own. Whatever answers
        # here, answers through USB.
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, nic=None, usbnic=True,
                 qemu="qemu-system-x86_64")
        try:
            boot = q.wait_serial("proton64>", timeout=90)
            check("system znajduje karte sieciowa na USB",
                  "Karta sieciowa USB (RNDIS)" in boot)
            check("i to ona jest karta tej maszyny",
                  "Siec: USB (RNDIS)" in boot)
            check("z adresem sprzetowym, ktory podala sama karta",
                  "52:54:0:12:34:56" in boot)

            got = q.wait_serial("DHCP: przyznano", timeout=60)
            check("DHCP przez USB: adres przyznany",
                  "przyznano 10.0.2.15" in got)

            out = q.command("ifconfig", "proton64>", timeout=25)
            check("ifconfig pokazuje karte USB jako eth0",
                  "USB (RNDIS)" in out and "10.0.2.15" in out)
            check("i licznik pakietow rusza sie w obie strony",
                  "RX 0 pakietow" not in out and "TX 0 pakietow" not in out)

            out = q.command("ping 10.0.2.2", "statystyki", timeout=40)
            check("ping przez karte USB dochodzi i wraca",
                  "odebrano 3" in out)

            # The whole way up: TCP through a USB cable, from ring 3
            out = q.command("run httpget 10.0.2.2 %d /" % port,
                            "kod wyjscia", timeout=60)
            check("HTTP przez karte USB: odpowiedz 200",
                  "[httpget] HTTP 200" in out)
            check("i tresc strony w calosci",
                  "ProtonOS TCP dziala" in out)
        finally:
            q.close()
            stop()
    finally:
        for path in (iso, slog, disk):
            if os.path.exists(path):
                os.unlink(path)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
