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
    print("--- the network card on USB (RNDIS) ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)

    try:
        check("the disc with the 64-bit kernel builds",
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
            check("the system finds the network card on USB",
                  "A USB network card (RNDIS)" in boot)
            check("and it is this machine's card",
                  "Network: USB (RNDIS)" in boot)
            check("with the hardware address the card itself gave",
                  "52:54:0:12:34:56" in boot)

            got = q.wait_serial("DHCP: granted", timeout=60)
            check("DHCP over USB: an address was granted",
                  "granted 10.0.2.15" in got)

            out = q.command("ifconfig", "proton64>", timeout=25)
            check("ifconfig shows the USB card as eth0",
                  "USB (RNDIS)" in out and "10.0.2.15" in out)
            check("and the packet counters move both ways",
                  "RX 0 packets" not in out and "TX 0 packets" not in out)

            out = q.command("ping 10.0.2.2", "statystyki", timeout=40)
            check("a ping over the USB card gets there and back",
                  "received 3" in out)

            # The whole way up: TCP through a USB cable, from ring 3
            out = q.command("run httpget 10.0.2.2 %d /" % port,
                            "exit code", timeout=60)
            check("HTTP over the USB card: a 200 answer",
                  "[httpget] HTTP 200" in out)
            check("and the page's content whole",
                  "ProtonOS TCP works" in out)
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
