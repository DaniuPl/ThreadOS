#!/usr/bin/env python3
"""Check legacy VirtIO-net PCI on both ThreadOS kernels."""

import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, make_iso64, no64, summary

VIRTIO_NIC = "virtio-net-pci,disable-modern=on"


def check_32():
    print("--- legacy VirtIO-net PCI (32-bit kernel) ---")
    slog = tempfile.mktemp(suffix=".log")
    q = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog,
             nic=VIRTIO_NIC)
    try:
        start = q.wait_for("Network:", timeout=40)
        check("the kernel finds the legacy VirtIO network device",
              re.search(r"Network: VirtIO-net ([0-9a-f]+:){5}", start)
              is not None)
        lease = q.wait_for("[net] DHCP", timeout=30)
        check("DHCP completes over VirtIO-net",
              "granted 10.0.2.15" in lease)

        q.type_line("ifconfig")
        ifc = q.wait_for("eth0  VirtIO-net", timeout=20)
        check("ifconfig identifies VirtIO-net",
              "eth0  VirtIO-net, MAC" in ifc)

        q.type_line("clear")
        q.type_line("ping 10.0.2.2")
        pong = q.wait_for("--- statistics:", timeout=35)
        check("VirtIO-net receives gateway echo replies",
              "sent 4, received 4" in pong)
    finally:
        q.close()
        if os.path.exists(slog):
            os.unlink(slog)


def check_64():
    print("--- legacy VirtIO-net PCI (64-bit kernel) ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    try:
        check("the disc with the 64-bit kernel builds",
              make_iso64(d, iso, text=True))
        if not os.path.exists(iso):
            return

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                 nic=VIRTIO_NIC, qemu="qemu-system-x86_64")
        try:
            start = q.wait_serial("Network:", timeout=90)
            check("the long-mode kernel finds the VirtIO network device",
                  re.search(r"Network: VirtIO-net ([0-9a-f]+:){5}", start)
                  is not None)
            lease = q.wait_serial("DHCP: granted", timeout=40)
            check("DHCP completes over VirtIO-net in long mode",
                  "granted 10.0.2.15" in lease)

            pong = q.command("ping 10.0.2.2", "--- statistics:",
                             timeout=40)
            check("the 64-bit kernel receives gateway echo replies",
                  "received 3" in pong or "received 4" in pong)
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for path in (iso, slog):
            if os.path.exists(path):
                os.unlink(path)


def main():
    check_32()
    check_64()
    return summary()


if __name__ == "__main__":
    sys.exit(main())
