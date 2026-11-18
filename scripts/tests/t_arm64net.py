#!/usr/bin/env python3
"""ThreadOS smoke test: the network on the ARM64 kernel.

The PC's network stack - kernel/net.c, tcp.c, dhcp.c: ARP, IP, ICMP,
UDP, TCP and the DHCP client - built for the ARM64 kernel, on the virt
machine's virtio network card (kernelarm64/vnet.c), with the PC's
system calls for it; and over them the C library's sockets
(libc/overlay/src/threados/socket.c), the same file as on the PC.

The program is the PC's nettest, and the host's side and every check
are t_socket.py's own: the same lines have to come out on both
kernels. The machine has QEMU's user network, as the PC's tests do:

  the card is found and DHCP is granted an address

  getaddrinfo asks the test's DNS server (named on the kernel's command
  line, dns=), and an unknown name is not found

  a TCP upload of 200000 bytes and 50000 more from a second thread at
  once, every byte checked by the server, and downloads checked by the
  program; a connect that does not wait, poll and SO_ERROR; EAGAIN;
  select timing out; ECONNREFUSED

  UDP datagrams answered, and poll timing out on a quiet one

  a server in the guest, which the host connects to through QEMU's
  port forwarding

on two cores, and the program ends with 0, every page of it free again.

Needs qemu-system-aarch64, `make kernelarm64`, and port 53 for the DNS
server (as t_socket does).
"""

import os
import re
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, ARM64_IMAGE, ARM64_INITRD, ARM64_READY,
                   start_dns_server, DNS_HINT)
from t_socket import Servers, free_port, judge, NAME


def boot(append, listen_port, log):
    return subprocess.Popen(
        ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72",
         "-m", "512", "-smp", "2", "-display", "none", "-no-reboot",
         "-monitor", "none", "-serial", "file:" + log,
         "-kernel", ARM64_IMAGE,
         "-fw_cfg", "name=opt/threados/initrd,file=" + ARM64_INITRD,
         "-global", "virtio-mmio.force-legacy=false",
         "-netdev", f"user,id=n0,hostfwd=tcp:127.0.0.1:{listen_port}-"
                    f":{listen_port}",
         "-device", "virtio-net-device,netdev=n0",
         "-append", append],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def said(log):
    if not os.path.exists(log):
        return ""
    with open(log, "rb") as f:
        return f.read().decode(errors="replace").replace("\r", "")


def wait_for(log, text, timeout, p):
    deadline = time.time() + timeout
    while time.time() < deadline and p.poll() is None:
        if text in said(log) or "stopping" in said(log):
            break
        time.sleep(0.2)
    return text in said(log)


def main():
    print("--- the network on the ARM64 kernel ---")
    why = noarm64()
    if why:
        print("SKIPPED: " + why)
        return summary()
    if not os.path.exists(ARM64_INITRD):
        print("SKIPPED: no ARM64 initrd (make kernelarm64)")
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
    log = tempfile.mktemp(suffix=".log")
    p = boot(f"dns=10.0.2.2 run=nettest,{NAME},{tcp_port},{udp_port},"
             f"{listen_port},{closed_port}", listen_port, log)
    reply = b""
    try:
        check("the virtio network card is found, and DHCP grants an address",
              wait_for(log, "[net] DHCP: granted 10.0.2.15", 60, p) and
              "Network: virtio-net" in said(log))
        if wait_for(log, f"listening on {listen_port}", 240, p):
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
        wait_for(log, ARM64_READY, 120, p)
    finally:
        if p.poll() is None:
            p.kill()
        p.wait()
        out = said(log)
        if os.path.exists(log):
            os.unlink(log)
        srv.close()
        dns_stop()

    start = re.search(r"^\[proc\] nettest started, pid \d+$", out, re.M)
    end = re.search(r"^\[proc\] nettest \(pid \d+\) ended with code (-?\d+)$",
                    out, re.M)
    body = out[start.end():end.start()] if start and end else out
    lines = [l for l in body.split("\n") if l and not l.startswith("[")]
    if os.environ.get("SOCKET_DEBUG"):
        print("\n".join(lines))
    judge(lines, srv, reply, tcp_port, listen_port)
    pages = re.search(r"pages free: (\d+) before the programs, (\d+) after",
                      out)
    check(f"nettest ends with code 0, every page free again "
          f"({pages.group(0) if pages else 'nothing said'})",
          end is not None and end.group(1) == "0" and pages is not None and
          pages.group(1) == pages.group(2))
    return summary()


if __name__ == "__main__":
    sys.exit(main())
