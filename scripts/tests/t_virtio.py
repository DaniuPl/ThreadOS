#!/usr/bin/env python3
"""Check legacy VirtIO-net PCI on the ThreadOS kernel."""

import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, make_iso64, no64, summary

VIRTIO_NIC = "virtio-net-pci,disable-modern=on"


def wait_regex(q, since, pattern, timeout=20):
    """What arrived on the serial port after 'since', once 'pattern'
    turns up in it (or when the time runs out - the assertion belongs
    to the caller). The port fills a character at a time, so a wait
    for the start of a line says nothing about the rest of it."""
    end = time.time() + timeout
    while time.time() < end:
        fresh = q.serial()[since:]
        if re.search(pattern, fresh):
            return fresh
        time.sleep(0.3)
    return q.serial()[since:]


def main():
    print("--- legacy VirtIO-net PCI ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    try:
        check("the disc with the 64-bit kernel builds",
              make_iso64(d, iso, text=True))
        if not os.path.exists(iso):
            return summary()

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                 nic=VIRTIO_NIC, qemu="qemu-system-x86_64")
        try:
            start = q.wait_serial("Network:", timeout=90)
            check("the kernel finds the legacy VirtIO network device",
                  re.search(r"Network: VirtIO-net ([0-9a-f]+:){5}", start)
                  is not None)
            # The address follows the words, so the wait goes on to the
            # comma that ends it
            q.wait_serial("DHCP: granted", timeout=40)
            lease = wait_regex(q, 0, r"DHCP: granted [0-9.]+,", timeout=20)
            check("DHCP completes over VirtIO-net",
                  "granted 10.0.2.15," in lease)

            q.wait_serial("thread64>", timeout=30)
            ifc = q.command("ifconfig", "RX ", timeout=20)
            check("ifconfig identifies VirtIO-net",
                  "eth0  VirtIO-net, MAC" in ifc)

            # The shell sends three echo requests; every one of them
            # has to come back. The wait is for the whole statistics
            # line, which reaches the port a character at a time.
            before = len(q.serial())
            q.type_line("ping 10.0.2.2")
            pong = wait_regex(q, before,
                              r"statistics: sent \d+, received \d+ ---",
                              timeout=40)
            check("VirtIO-net receives gateway echo replies",
                  "sent 3, received 3 ---" in pong)
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for path in (iso, slog):
            if os.path.exists(path):
                os.unlink(path)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
