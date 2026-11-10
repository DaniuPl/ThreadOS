#!/usr/bin/env python3
"""ThreadOS smoke test: the e1000 network card (Intel 82540EM).

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

We check both directions of traffic - receiving with no reception means
nothing, and sending with no answer proves nothing beyond that something
went out on the wire.
"""

import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def wait_regex(q, since, pattern, timeout=20):
    """What arrived on the serial port after 'since', once 'pattern'
    turns up in it (or when the time runs out - the assertion belongs
    to the caller)."""
    end = time.time() + timeout
    while time.time() < end:
        fresh = q.serial()[since:]
        if re.search(pattern, fresh):
            return fresh
        time.sleep(0.3)
    return q.serial()[since:]


def main():
    # The card's registers are in memory, high above what the kernel
    # maps at boot, so the driver maps them itself before it touches
    # them - a step the RTL8139's I/O ports never needed
    print("--- the e1000 card ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog64 = tempfile.mktemp(suffix=".log")
    try:
        check("the disc with the 64-bit kernel builds", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        http_port, http_stop = start_http_server()
        q64 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog64, nic="e1000",
                   qemu="qemu-system-x86_64")
        try:
            start = q64.wait_serial("Network:", timeout=90)
            check("the kernel finds the e1000 card and says which it is",
                  re.search(r"Network: e1000 ([0-9a-f]+:){5}", start) is not None)

            # DHCP is a full round trip: a question by broadcast, the
            # server's answer, accepting the lease. The address comes
            # after the words, a character at a time, so the wait goes
            # on to the comma that ends it.
            q64.wait_serial("DHCP: granted", timeout=40)
            lease = wait_regex(q64, 0, r"DHCP: granted [0-9.]+,", timeout=20)
            check("DHCP over the e1000 granted an address",
                  "granted 10.0.2.15," in lease)

            q64.wait_serial("thread64>", timeout=30)
            ifc = q64.command("ifconfig", "RX ", timeout=20)
            check("ifconfig names the card",
                  "eth0  e1000, MAC" in ifc)

            # ICMP: slirp's gateway answers, so the frame really went out
            # and really came back
            before = len(q64.serial())
            q64.type_line("ping 10.0.2.2")
            pong = wait_regex(q64, before,
                              r"statistics: sent \d+, received \d+ ---",
                              timeout=40)
            check("ping over the e1000: an ICMP echo answer from the gateway",
                  "40 B from 10.0.2.2" in pong)
            check("ping over the e1000: every packet came back",
                  "sent 3, received 3" in pong)

            # The address and the MAC after it are two writes to the
            # port, so the wait is for the whole entry, not the address
            gateway = r"10\.0\.2\.2\s+([0-9a-f]+:){5}[0-9a-f]+"
            before = len(q64.serial())
            q64.type_line("arp")
            arp = wait_regex(q64, before, gateway, timeout=20)
            check("the ARP table knows the gateway's hardware address",
                  re.search(gateway, arp) is not None)

            # TCP to the host from ring 3: the longest path this card has
            # to travel
            page = q64.command("run httpget 10.0.2.2 %d /" % http_port,
                               "exit code", timeout=60)
            check("TCP over the e1000: an HTTP 200 answer with the whole content",
                  "[httpget] HTTP 200" in page and
                  "ThreadOS TCP works" in page)

            count = q64.command("ifconfig", "dropped", timeout=20)
            m = re.search(r"RX (\d+) packets, TX (\d+) packets", count)
            check("the counters show traffic both ways",
                  m is not None and int(m.group(1)) > 10 and int(m.group(2)) > 10)
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
