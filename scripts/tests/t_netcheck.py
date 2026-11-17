#!/usr/bin/env python3
"""ThreadOS smoke test: what the network stack takes in, frame by frame.

The kernel's network code reads whatever arrives off the wire. A review
of it found that it believed every frame: no IP header checksum, no TCP
or UDP or ICMP checksum, fragments read as if their first bytes were a
TCP header, a ping to the broadcast address answered (one forged ping,
a reply from every machine at its victim), TCP's first sequence number
the uptime times a constant, and six connections for the whole system.

Here the machine's card sits on a hub with QEMU's user network, which
gives it its address as always, and with a UDP socket of this test's:
frames made here, byte by byte, reach the card as if off the wire, and
everything the system sends comes back here too. From outside:

  a well-made ping is answered - and one with a wrong IP header
  checksum, one with a wrong ICMP checksum, a first fragment, a later
  fragment, and one to the broadcast address are not; a well-made one
  after all that is answered again

  a SYN to a closed port is answered with an RST; the same with a
  wrong TCP checksum is not

  the first sequence numbers of the connections a listener queues
  (user64/hostile64.c listen) are not the uptime times 2654435761: undone
  by that constant's inverse they come out as large as any number, not
  as a few thousand timer ticks

  a program opens ten connections at once to a server of the test's
  (it got six before)

Run on its own or through scripts/run_tests.py.
"""

import os
import random
import re
import socket
import struct
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, _kernel_define

# Connections a listener holds before its program accepts them; a SYN
# past that is let go, to be sent again
ACCEPT_QUEUE = _kernel_define("kernel/tcp.c", "ACCEPT_QUEUE")

GUEST_MAC = bytes.fromhex("525400123456")      # QEMU's first card
OUR_MAC = bytes.fromhex("525400aabb01")
GUEST_IP = bytes([10, 0, 2, 15])
OUR_IP = bytes([10, 0, 2, 77])
BROADCAST = bytes([255, 255, 255, 255])


def inet_sum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def ip_frame(proto, payload, dst=GUEST_IP, frag=0, bad_header=False):
    hdr = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), 0x1234,
                      frag, 64, proto, 0, OUR_IP, dst)
    c = inet_sum(hdr) ^ (0x5555 if bad_header else 0)
    hdr = hdr[:10] + struct.pack("!H", c) + hdr[12:]
    return GUEST_MAC + OUR_MAC + b"\x08\x00" + hdr + payload


def echo(ident, bad=False):
    body = struct.pack("!BBHHH", 8, 0, 0, ident, 1) + b"ThreadOS netcheck"
    c = inet_sum(body) ^ (0x00FF if bad else 0)
    return body[:2] + struct.pack("!H", c) + body[4:]


def syn(sport, dport, seq, bad=False):
    seg = struct.pack("!HHIIBBHHH", sport, dport, seq, 0, 0x50, 0x02, 65535,
                      0, 0)
    pseudo = OUR_IP + GUEST_IP + struct.pack("!BBH", 0, 6, len(seg))
    c = inet_sum(pseudo + seg) ^ (0x0F0F if bad else 0)
    return seg[:16] + struct.pack("!H", c) + seg[18:]


class Wire:
    """The test's end of the hub: frames to the card, and everything the
    card sends, kept with the time it came"""

    def __init__(self):
        self.rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rx.bind(("127.0.0.1", 0))
        self.theirs = self.rx.getsockname()[1]
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.bind(("127.0.0.1", 0))
        self.ours = probe.getsockname()[1]
        probe.close()                  # QEMU binds it
        self.frames = []
        self.lock = threading.Lock()
        self.running = True
        threading.Thread(target=self._listen, daemon=True).start()

    def _listen(self):
        self.rx.settimeout(0.5)
        while self.running:
            try:
                data, _ = self.rx.recvfrom(65536)
            except socket.timeout:
                continue
            except OSError:
                return
            with self.lock:
                self.frames.append((time.time(), data))

    def send(self, frame):
        self.rx.sendto(frame, ("127.0.0.1", self.ours))

    def wait(self, pred, since, timeout=4.0):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                for t, f in self.frames:
                    if t >= since and pred(f):
                        return f
            time.sleep(0.1)
        return None

    def close(self):
        self.running = False
        self.rx.close()


def ipv4(f):
    """(proto, src ip, ihl, payload) of an IPv4 frame from the guest"""
    if len(f) < 34 or f[12:14] != b"\x08\x00" or f[26:30] != GUEST_IP:
        return None
    ihl = (f[14] & 15) * 4
    return f[23], f[26:30], f[14 + ihl:]


def echo_reply(ident):
    def pred(f):
        p = ipv4(f)
        return (p and p[0] == 1 and len(p[2]) >= 8 and p[2][0] == 0 and
                struct.unpack("!H", p[2][4:6])[0] == ident)
    return pred


def tcp_to(sport, flags_mask):
    def pred(f):
        p = ipv4(f)
        return (p and p[0] == 6 and len(p[2]) >= 20 and
                struct.unpack("!H", p[2][2:4])[0] == sport and
                p[2][13] & flags_mask == flags_mask)
    return pred


def pings(wire):
    def ping(ident, frame):
        t = time.time()
        wire.send(frame)
        return wire.wait(echo_reply(ident), t, timeout=3) is not None

    check("a well-made ping is answered", ping(0x1001, ip_frame(1, echo(0x1001))))
    check("one with a wrong IP header checksum is not",
          not ping(0x1002, ip_frame(1, echo(0x1002), bad_header=True)))
    check("one with a wrong ICMP checksum is not",
          not ping(0x1003, ip_frame(1, echo(0x1003, bad=True))))
    check("a first fragment (More Fragments) is not taken for a whole one",
          not ping(0x1004, ip_frame(1, echo(0x1004), frag=0x2000)))
    check("nor a later piece (an offset)",
          not ping(0x1005, ip_frame(1, echo(0x1005), frag=0x0001)))
    check("a ping to the broadcast address is not answered (smurf)",
          not ping(0x1006, ip_frame(1, echo(0x1006), dst=BROADCAST)))
    check("and a well-made one after all that is answered again",
          ping(0x1007, ip_frame(1, echo(0x1007))))


def resets(wire):
    t = time.time()
    wire.send(ip_frame(6, syn(40001, 9, 1000)))
    check("a SYN to a closed port is answered with an RST",
          wire.wait(tcp_to(40001, 0x04), t) is not None)
    t = time.time()
    wire.send(ip_frame(6, syn(40002, 9, 1000, bad=True)))
    check("the same with a wrong TCP checksum is not",
          wire.wait(tcp_to(40002, 0x04), t, timeout=3) is None)


def first_numbers(q, wire):
    q.type_line("run hostile64 listen 7070 15")
    q.wait_serial("[hostile64] listening on 7070", timeout=60)
    seqs = []
    for i in range(ACCEPT_QUEUE):
        sport = 41001 + i
        t = time.time()
        wire.send(ip_frame(6, syn(sport, 7070, 5000 + i)))
        f = wire.wait(tcp_to(sport, 0x12), t)
        if f:
            seqs.append(struct.unpack("!I", ipv4(f)[2][4:8])[0])
    check("a listener answers %d SYNs, as many as it queues, with "
          "SYN-ACKs (%d)" % (ACCEPT_QUEUE, len(seqs)),
          len(seqs) == ACCEPT_QUEUE)
    inv = pow(2654435761, -1, 1 << 32)
    undone = [s * inv % (1 << 32) for s in seqs]
    check("whose first sequence numbers are not the uptime times "
          "2654435761 (undone: %s)" % ", ".join(str(u) for u in undone),
          len(undone) == ACCEPT_QUEUE and all(u > 10 ** 7 for u in undone)
          and len(set(seqs)) == ACCEPT_QUEUE)
    q.wait_serial("[hostile64] listened", timeout=60)


def connections(q):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", 0))
    srv.listen(32)
    port = srv.getsockname()[1]
    held = []
    stop = threading.Event()

    def accept():
        srv.settimeout(0.5)
        while not stop.is_set():
            try:
                c, _ = srv.accept()
                held.append(c)
            except socket.timeout:
                continue
            except OSError:
                return
    threading.Thread(target=accept, daemon=True).start()
    try:
        out = q.command("run hostile64 conns 10 10.0.2.2 %d" % port,
                        "connections open at once", timeout=120)
        m = re.search(r"connections open at once: (\d+) of 10", out)
        check("a program opens ten connections at once (%s)"
              % (m.group(1) if m else "-"),
              m is not None and m.group(1) == "10")
    finally:
        stop.set()
        srv.close()
        for c in held:
            c.close()


def main():
    print("--- what the network stack takes in ---")
    wire = Wire()
    log = tempfile.mktemp(suffix=".log")
    try:
        q = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=log,
                 rawnet=(wire.ours, wire.theirs))
        try:
            q.wait_serial("thread64>", timeout=90)
            q.wait_serial("DHCP: granted 10.0.2.15", timeout=60)
            pings(wire)
            resets(wire)
            first_numbers(q, wire)
            connections(q)
        finally:
            q.close()
    finally:
        wire.close()
        if os.path.exists(log):
            os.unlink(log)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
