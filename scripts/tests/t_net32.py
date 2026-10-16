#!/usr/bin/env python3
"""ThreadOS smoke test: the network in the 32-bit kernel (ARP, ICMP,
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
        q.wait_for("ThreadOS shell", timeout=40)
        # The IP address comes from slirp's DHCP server only after the
        # shell has started - without waiting for it every network test
        # would be a race with the lease
        q.wait_for("[net] DHCP", timeout=30)
        time.sleep(0.5)

        q.type_line("ifconfig")
        ifc = q.wait_for("eth0  RTL8139, MAC")
        check("ifconfig names the card that answered",
              "eth0  RTL8139, MAC" in ifc)
        check("ifconfig shows the IP address", "IP 10.0.2.15" in ifc)
        check("ifconfig shows the gateway", "gateway 10.0.2.2" in ifc)
        check("ifconfig shows the DNS server from DHCP", "DNS 10.0.2.3" in ifc)

        q.type_line("ping 10.0.2.2")
        q.type_line("dns 10.0.2.3")     # typing ahead during the ping
        pong = q.wait_for("40 B od 10.0.2.2: seq=1", timeout=20)
        check("ping: an ICMP echo answer from the gateway",
              "40 B od 10.0.2.2" in pong)
        stats = q.wait_for("--- statistics:", timeout=20)
        check("ping: all 4 packets came back",
              "sent 4, received 4" in stats)
        buffered = q.wait_for("DNS server set to", timeout=20)
        check("characters typed during a command are not lost (the keyboard buffer)",
              "DNS server set to 10.0.2.3" in buffered)

        q.type_line("arp")
        check("the ARP table knows the gateway's MAC",
              "10.0.2.2" in q.wait_for("10.0.2.2  "))

        # UDP sockets in ring 3: two processes talk through the loopback,
        # so the test does not depend on any network outside
        q.type_line("run udpecho")
        view = q.wait_for("listening on port 7777", timeout=10)
        check("a ring 3 process opened a UDP socket",
              "listening on port 7777" in view)

        q.type_line("run udptest")
        echo = q.wait_for("the echo came back", timeout=10)
        check("the datagram came back to the sender through the loopback",
              "the echo came back: thread-udp" in echo)
        check("the server saw the client's datagram",
              "[udpecho] od 10.0.2.15:" in echo)
        check("the client ended with no error",
              "process \'udptest\' has ended (code 0)" in echo)

        # A socket has an owner: after the process dies the port has to be
        # freed
        q.type_line("kill udpecho")
        time.sleep(0.5)
        q.type_line("clear")          # a clear screen: the old "listening" disappears
        q.type_line("run udpecho")
        again = q.wait_for("port 7777", timeout=10)
        check("the port was freed once the socket's owner died",
              "listening on port 7777" in again
              and "the socket could not be opened" not in again)
        q.type_line("kill udpecho")

        # DNS from ring 3. We ask our own server standing on the host
        # (10.0.2.2), so the test depends neither on the internet nor on
        # the machine's resolver.
        dns_stop, dns_ok = start_dns_server()
        if not dns_ok:
            print("SKIPPED (DNS): " + DNS_HINT)
        else:
            try:
                q.type_line("clear")
                q.type_line("dns 10.0.2.2")
                q.wait_for("DNS server set", timeout=10)
                q.type_line("run nslookup threados.test")
                dns = q.wait_for("has the address", timeout=20)
                check("nslookup resolved the name through the named DNS server",
                      "threados.test has the address 10.0.2.2" in dns)
            finally:
                dns_stop()

        q.type_line("ifconfig")
        ifc = q.wait_for("RX ")
        # The output of the previous 'ifconfig' (from before the ping)
        # hangs on the screen too - what counts is the last, freshest
        # occurrence
        rx = re.findall(r"RX (\d+) packets, TX (\d+) packets", ifc)
        check(f"the RX/TX packet counters grow"
              f" (the last: {rx[-1] if rx else None})",
              bool(rx) and int(rx[-1][0]) > 0 and int(rx[-1][1]) > 0)
    finally:
        q.close()
        if os.path.exists(disk):
            os.unlink(disk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
