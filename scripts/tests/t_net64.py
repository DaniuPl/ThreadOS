#!/usr/bin/env python3
"""ThreadOS smoke test: the network (ARP, ICMP, UDP, TCP), from the
kernel's shell and from a program in ring 3.

The kernel's own commands come first: 'ifconfig' with what DHCP gave,
'ping' to slirp's gateway and the ARP table. Then the path through
system calls: a program in ring 3 asks for its own address, sends a
datagram to itself and connects to a server over TCP, and two programs
- a UDP server and its client - exchange a datagram through the
loopback.

One assertion is not about the network at all and is here on purpose:
'ping' sleeps in the shell's thread for over a second, and a command
typed during that time is to wait in the keyboard buffer and run right
after it. In the old days the shell ran in interrupt context, so it
could not sleep.

Everything here talks to slirp's gateway (10.0.2.2), to a server the
test itself puts up on the host, or to an address that never answers,
so none of the checks depends on whether the machine this runs on has
internet.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64
from ptest import _kernel_define as define

PROMPT = "thread64>"
# TEST-NET-1 (RFC 5737): reserved for documentation, so nothing there
# ever answers a question - a program asking it waits out its timeout
SILENT = "192.0.2.1"


# The shell's report on a program's end, as kernel64/shell64.c prints it
REPORT = (r"\[%s\] done, exit code (-?\d+) \(frames \d+ -> \d+, "
          r"heap pages \d+, stack \d+, from files \d+\)\r?\n")


def wait_fresh(q, start, text, timeout):
    """What arrived on the serial port since 'start', once 'text' is in
    it (or when the time is up)."""
    end = time.time() + timeout
    while time.time() < end:
        fresh = q.serial()[start:]
        if text in fresh:
            return fresh
        time.sleep(0.3)
    return q.serial()[start:]


def wait_fresh_re(q, start, pattern, timeout):
    """The same for a pattern - one that runs to the line's end waits
    for the whole line, where a text it starts with could be read while
    the rest of the line is still on its way."""
    end = time.time() + timeout
    while time.time() < end:
        fresh = q.serial()[start:]
        if re.search(pattern, fresh):
            return fresh
        time.sleep(0.3)
    return q.serial()[start:]


def run(q, line, timeout):
    """Runs a program from the kernel's shell and hands back what arrived
    after the command, once the shell's whole report on its end is
    there."""
    start = len(q.serial())
    q.type_line(line)
    return wait_fresh_re(q, start, REPORT % re.escape(line.split()[1]),
                         timeout)


def exit_code(text, prog):
    m = re.search(REPORT % re.escape(prog), text)
    return int(m.group(1)) if m else None


def main():
    print("--- the network: the kernel's commands and a program's side ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return 0

    http_port, http_stop = start_http_server()
    # Two answers for one file name: the longer first, then the shorter
    # over it - what is left must be the shorter one and nothing else
    LONG = b"the first download, which is the longer of the two\n" * 4
    SHORT = b"the second, shorter\n"
    SERVED["/long.txt"] = LONG
    SERVED["/short.txt"] = SHORT
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        # On QEMU's user network the gateway 10.0.2.2 IS the host, so the
        # program connects to the server standing right here without any
        # forwarding - those are for traffic the other way
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, sata=True)
        try:
            q.wait_serial(PROMPT, timeout=60)
            # The IP address comes from slirp's DHCP server only after
            # the shell has started - without waiting for it every
            # network test would be a race with the lease
            q.wait_serial("[net] DHCP: granted", timeout=40)

            # --- The kernel's commands ---
            ifc = q.command("ifconfig", "RX ", timeout=20)
            check("ifconfig names the card that answered",
                  "eth0  RTL8139, MAC" in ifc)
            check("ifconfig shows the IP address", "IP 10.0.2.15" in ifc)
            check("ifconfig shows the gateway", "gateway 10.0.2.2" in ifc)
            check("ifconfig shows the DNS server from DHCP",
                  "DNS 10.0.2.3" in ifc)

            seen = len(q.serial())
            q.type_line("ping 10.0.2.2")
            # Typing ahead during the ping. The three answers take over
            # a second, typing a whole line about as long - so the
            # first letter is looked at alone: it has to go in while
            # the ping still runs, or nothing is typed ahead at all.
            q.sendkey("d")
            during = "--- statistics" not in q.serial()[seen:]
            q.type_line("ns 10.0.2.3")
            pong = wait_fresh(q, seen, "DNS server set to", timeout=40)
            check("ping: an ICMP echo answer from the gateway",
                  "40 B from 10.0.2.2: seq=1" in pong)
            check("ping: all 3 packets came back",
                  "--- statistics: sent 3, received 3 ---" in pong)
            stats = pong.find("--- statistics:")
            check("characters typed during a command are not lost (the "
                  "keyboard buffer): typed while the ping ran (%s), the "
                  "command ran whole after it" % during,
                  during and stats >= 0 and
                  pong.find("DNS server set to 10.0.2.3", stats) > stats)

            arp = q.command("arp", PROMPT, timeout=20)
            check("the ARP table knows the gateway's MAC",
                  re.search(r"10\.0\.2\.2  [0-9a-f]{1,2}(:[0-9a-f]{1,2}){5}",
                            arp) is not None)

            # --- A program's side: system calls ---
            out = run(q, f"run unet64 10.0.2.2 {http_port}", timeout=60)

            m = re.search(r"my address (\d+\.\d+\.\d+\.\d+), "
                          r"gateway (\d+\.\d+\.\d+\.\d+)", out)
            check("the program asks the kernel for its DHCP address"
                  + (f" ({m.group(1)})" if m else ""),
                  m is not None and m.group(1) == "10.0.2.15")

            # The loopback does not go out on the wire, so this attempt
            # checks the stack itself, not the network around it. The
            # address printed is the sender's as the kernel wrote it back
            # into the program's description of the socket. unet64 clears
            # the address there after sending, so what it prints is not
            # its own left over: a kernel that wrote nothing back would
            # have it print 0.0.0.0.
            m = re.search(r"the datagram came back to me \((\d+) B\) "
                          r"from (\d+\.\d+\.\d+\.\d+)", out)
            check("a datagram sent to itself comes back the same"
                  + (f" ({m.group(1)} B from {m.group(2)})" if m else ""),
                  m is not None and int(m.group(1)) > 0
                  and m.group(2) == "10.0.2.15")

            m = re.search(r"the server answered \((\d+) B\): (HTTP/1\.\d)",
                          out)
            check("a TCP connection from ring 3 gets the server's answer"
                  + (f" ({m.group(2)}, {m.group(1)} B)" if m else ""),
                  m is not None)

            check("the program ends with no error",
                  exit_code(out, "unet64") == 0)
            check("no attempt reported an error",
                  "[unet64] FAILED" not in out)

            # --- UDP between two processes ---
            # Two sockets now, in two programs: a server on a port of
            # its own, started in the background, and a client on a port
            # the kernel picks. Each learns the other's address and port
            # only from what the kernel writes back on receiving, into a
            # description that held zeros: a kernel that wrote nothing
            # back would have the server print 0.0.0.0:0 and answer
            # nobody.
            seen = len(q.serial())
            q.type_line("runw udpecho")
            srv = wait_fresh(q, seen, "[udpecho] listening on port 7777",
                             timeout=30)
            check("a ring 3 server opened a UDP socket on its port",
                  "[udpecho] listening on port 7777" in srv)
            cli = run(q, "run udptest", timeout=60)
            m = re.search(r"\[udpecho\] from (\d+\.\d+\.\d+\.\d+):(\d+) -> "
                          r"'thread-udp'", cli)
            ephemeral = define("kernel/net.c", "UDP_PORT_MIN")
            check("the server saw the client's datagram, from the client's "
                  "own address and a port the kernel picked for it"
                  + (f" ({m.group(1)}:{m.group(2)})" if m else ""),
                  m is not None and m.group(1) == "10.0.2.15"
                  and ephemeral <= int(m.group(2)) <= 0xFFFF)
            check("the echo came back to the sender, from the server's port",
                  "[udptest] the echo came back: thread-udp "
                  "(from 10.0.2.15:7777)" in cli)
            check("the client ended with no error",
                  exit_code(cli, "udptest") == 0)
            # The server gives up after a while with nobody writing to it.
            # Its port has to be free again before 'unet64' below wants
            # it, and the count says the client's datagram was the only
            # one it got.
            end = wait_fresh_re(q, seen, r"\[udpecho\] done, datagrams "
                                         r"served: \d+\r?\n", timeout=60)
            check("the server ended by itself after serving one datagram",
                  re.search(r"\[udpecho\] done, datagrams served: 1\r?\n",
                            end) is not None)

            # --- A socket has an owner ---
            # A program killed while it waits on a UDP socket does not
            # close it; the kernel has to, once the owner is dead. Each
            # nslookup below asks a server that never answers and is
            # stopped with Ctrl+C in the middle of the wait - as many
            # times as the kernel has sockets. Were the dead owners'
            # sockets not freed, the table would be full and no port
            # could be opened after them.
            q.command("dns " + SILENT, "DNS server set", timeout=20)
            socks = define("kernel/net.c", "UDP_SOCKS")
            killed = 0
            for _ in range(socks):
                seen = len(q.serial())
                q.type_line("run nslookup threados.test")
                wait_fresh(q, seen, "[nslookup] asking %s about threados.test"
                           % SILENT, timeout=30)
                q.sendkey("ctrl-c")
                end = wait_fresh_re(q, seen, REPORT % "nslookup",
                                    timeout=30)
                if ("(nslookup) killed: a signal" in end and
                        exit_code(end, "nslookup") == -1 and
                        "no answer" not in end):
                    killed += 1
            check("Ctrl+C killed nslookup while it waited on its socket, "
                  "%d of %d times" % (killed, socks), killed == socks)
            out = run(q, "run unet64", timeout=60)
            check("the sockets were freed once their owners died: port "
                  "7777 opens after %d killed owners" % socks,
                  "the datagram came back to me" in out and
                  exit_code(out, "unet64") == 0)

            # A download over a file that is already there replaces it:
            # the file is emptied when it is opened, not just written
            # from its start (O_TRUNC, which the 64-bit side of the
            # shared programs used to drop on the way to the kernel)
            for name in ("long.txt", "short.txt"):
                run(q, f"run httpget 10.0.2.2 {http_port} /{name} got.txt",
                    timeout=60)
            out = run(q, "run ucat64 got.txt", timeout=30)
            check("a shorter download over a longer file leaves only the "
                  "shorter one", SHORT.decode().strip() in out and
                  "the first download" not in out)

            seen = len(q.serial())
            q.type_line("ifconfig")
            ifc = wait_fresh_re(q, seen, r"RX \d+ packets, TX \d+ packets, "
                                         r"dropped \d+\+\d+\r?\n", timeout=20)
            rx = re.findall(r"RX (\d+) packets, TX (\d+) packets", ifc)
            check(f"the RX/TX packet counters grow"
                  f" (the last: {rx[-1] if rx else None})",
                  bool(rx) and int(rx[-1][0]) > 0 and int(rx[-1][1]) > 0)
        finally:
            q.close()
    finally:
        http_stop()
        if os.path.exists(disk):
            os.unlink(disk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
