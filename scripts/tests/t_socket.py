#!/usr/bin/env python3
"""ThreadOS smoke test: BSD sockets for musl programs, 64-bit kernel.

musl answered ENOSYS to socket(), so no networking library could be
brought in. Sockets are now descriptors made by the C library over the
kernel's TCP and UDP (libc/overlay/src/threados/socket.c), with poll()
and select() over sockets and files alike, and the network's name
server behind getaddrinfo().

The other end of every conversation is this test, on the host, and it
checks what arrived independently of the guest:

  getaddrinfo() asks the test's DNS server (so the name resolves to the
  address the server gave, and an unknown name does not)

  a TCP upload of 200000 bytes, and 50000 more from a second thread at
  the same time: the server checks every byte against the pattern, and
  sends a sum and a download of its own, which the program checks

  a connect that does not wait, followed by poll() and SO_ERROR; a read
  with nothing to read (EAGAIN); select() timing out; a port nobody
  listens on (ECONNREFUSED)

  UDP datagrams answered by the server, and poll() timing out on a
  quiet datagram socket

  a server in the guest: the host connects to it through QEMU's port
  forwarding, and gets an answer only the guest could have made

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import socket
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64, start_dns_server, DNS_HINT

PROMPT = "thread64>"
NAME = "echo.threados.test"


def pattern(i, salt):
    return (i * 31 + (i >> 8) * 7 + salt) & 255


def fnv(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Servers:
    """The host's side: a TCP server that checks uploads, and a UDP echo"""

    def __init__(self):
        self.uploads = []            # (n, salt, intact)
        self.datagrams = 0
        self.stop = threading.Event()
        self.tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.tcp.bind(("0.0.0.0", 0))
        self.tcp.listen(8)
        self.tcp.settimeout(0.5)
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(("0.0.0.0", 0))
        self.udp.settimeout(0.5)
        threading.Thread(target=self.accept_loop, daemon=True).start()
        threading.Thread(target=self.udp_loop, daemon=True).start()

    def accept_loop(self):
        while not self.stop.is_set():
            try:
                c, _ = self.tcp.accept()
            except (socket.timeout, OSError):
                continue
            threading.Thread(target=self.serve, args=(c,), daemon=True).start()

    def serve(self, c):
        c.settimeout(60)
        try:
            line = b""
            while not line.endswith(b"\n"):
                b = c.recv(1)
                if not b:
                    return           # a connection that never asks
                line += b
            _, n, back, salt = line.split()
            n, back, salt = int(n), int(back), int(salt)
            data = b""
            while len(data) < n:
                b = c.recv(65536)
                if not b:
                    break
                data += b
            intact = len(data) == n and all(
                data[i] == pattern(i, salt) for i in range(n))
            self.uploads.append((n, salt, intact))
            c.sendall(b"OK %08x\n" % fnv(data))
            c.sendall(bytes(pattern(i, salt + 1) for i in range(back)))
        except OSError:
            pass
        finally:
            c.close()

    def udp_loop(self):
        while not self.stop.is_set():
            try:
                data, addr = self.udp.recvfrom(2048)
            except (socket.timeout, OSError):
                continue
            self.datagrams += 1
            self.udp.sendto(b"echo: " + data, addr)

    def close(self):
        self.stop.set()
        self.tcp.close()
        self.udp.close()


def judge(lines, srv, reply, tcp_port, listen_port):
    """What nettest printed and the host's side saw - the same on every
    kernel it runs on (t_arm64net.py runs it on the ARM64 one)"""
    def has(text):
        return text in lines

    check(f"getaddrinfo asks the network's DNS: {NAME} is 10.0.2.2",
          has(f"resolved {NAME} to 10.0.2.2 port {tcp_port}"))
    check("and a name the server does not know is not found",
          has("a name nobody knows: not found"))
    check("a TCP upload: the server's sum of 200000 bytes matches the "
          "program's, and 100000 bytes came back intact",
          has("tcp: sent 200000, the server's sum matches mine, got back "
              "100000 bytes intact"))
    check("a second thread on its own connection at the same time",
          has("a second thread: sent 50000, the server's sum matches "
              "mine, got back 30000 bytes intact"))
    check(f"the server found every byte it was sent where it should be "
          f"({srv.uploads})",
          sorted(u[:2] for u in srv.uploads if u[2]) ==
          [(50000, 77), (200000, 3)])
    check("a connect that does not wait: EINPROGRESS, then poll says "
          "writable and SO_ERROR is 0",
          has("nonblocking connect: EINPROGRESS, then poll 1 writable, "
              "SO_ERROR 0"))
    check("a read with nothing to read does not wait",
          has("a read with nothing there: EAGAIN"))
    check("select on a quiet socket times out after its 300 ms",
          has("select on a quiet socket: 0 after about 300 ms"))
    check("a port nobody listens on is refused",
          has("a port nobody listens on: ECONNREFUSED"))
    check(f"UDP: three datagrams answered ({srv.datagrams} reached the "
          "server)",
          has("udp: 3 of 3 datagrams answered, from the port they went "
              "to") and srv.datagrams == 3)
    check("poll on a quiet datagram socket times out after its 300 ms",
          has("poll on a quiet datagram socket: 0 after about 300 ms"))
    check(f"a server in the guest answers the host ({reply!r})",
          reply == b"HELLO FROM THE HOST\n")
    check("and sees the host's side of QEMU's network, on its own port",
          has(f"accepted a connection from 10.0.2.2, to my port "
              f"{listen_port}"))


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    dns_stop, dns_ok = start_dns_server(only="threados.test")
    if not dns_ok:
        print("SKIPPED: " + DNS_HINT)
        return summary()

    srv = Servers()
    tcp_port = srv.tcp.getsockname()[1]
    udp_port = srv.udp.getsockname()[1]
    listen_port = free_port()
    closed_port = free_port()
    work = tempfile.mkdtemp()
    try:
        iso = os.path.join(work, "t.iso")
        slog = os.path.join(work, "serial.log")
        check("the disc with the 64-bit kernel builds",
              make_iso64(os.path.join(work, "iso"), iso))
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64", hostfwd_port=listen_port, cpus=2)
        try:
            q.wait_serial(PROMPT, timeout=90)
            q.wait_serial("[net] DHCP", timeout=60)
            # The system's name server is the test's own: the C library's
            # resolver has to follow the system's setting, not DHCP's
            q.command("dns 10.0.2.2", "DNS server set")
            seen = len(q.serial())
            q.type_line(f"run nettest {NAME} {tcp_port} {udp_port} "
                        f"{listen_port} {closed_port}")
            try:
                q.wait_serial(f"listening on {listen_port}", timeout=240)
            except RuntimeError:
                if os.environ.get("SOCKET_DEBUG"):
                    print(q.serial()[seen:])
                raise
            reply = b""
            for _ in range(20):
                try:
                    with socket.create_connection(("127.0.0.1", listen_port),
                                                  timeout=10) as c:
                        c.sendall(b"hello from the host\n")
                        while not reply.endswith(b"\n"):
                            b = c.recv(100)
                            if not b:
                                break
                            reply += b
                    break
                except OSError:
                    time.sleep(0.5)
            q.wait_serial("[nettest] done, exit code", timeout=60)
            out = q.serial()[seen:].replace("\r", "")
        finally:
            q.close()

        lines = [l for l in out.split("\n") if l and not l.startswith(
                 ("[ OK ]", "[kernel]", "run nettest", PROMPT))]
        if os.environ.get("SOCKET_DEBUG"):
            print("\n".join(lines))

        judge(lines, srv, reply, tcp_port, listen_port)
        check("nettest ends with code 0",
              "[nettest] done, exit code 0" in out)
    finally:
        srv.close()
        dns_stop()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
