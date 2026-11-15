#!/usr/bin/env python3
"""ThreadOS smoke test: the legacy VirtIO-net PCI network device."""

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


def wait_shell(q, timeout=90):
    """The first console's prompt. The other three consoles print theirs
    earlier, so the first 'thread64>' on the port does not mean the
    shell the keyboard talks to is there - and a command waiting for the
    prompt after it would take the first console's for its own."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        out = q.serial()
        at = out.find("Commands:")
        if at >= 0 and "thread64>" in out[at:]:
            return out
        time.sleep(0.3)
    raise RuntimeError("timeout: the first console's shell never came up")


def check_virtio():
    print("--- legacy VirtIO-net PCI ---")
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
            check("the kernel finds the legacy VirtIO network device",
                  re.search(r"Network: VirtIO-net ([0-9a-f]+:){5}", start)
                  is not None)
            lease = q.wait_serial("DHCP: granted", timeout=40)
            check("DHCP completes over VirtIO-net",
                  "granted 10.0.2.15" in lease)

            # The prompt comes after the whole answer
            wait_shell(q)
            ifc = q.command("ifconfig", "thread64>", timeout=20)
            check("ifconfig identifies VirtIO-net",
                  "eth0  VirtIO-net, MAC" in ifc)

            pong = q.command("ping 10.0.2.2", "--- statistics:",
                             timeout=40)
            check("VirtIO-net receives every gateway echo reply",
                  "sent 3, received 3" in pong)
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for path in (iso, slog):
            if os.path.exists(path):
                os.unlink(path)


def main():
    check_virtio()
    return summary()


if __name__ == "__main__":
    sys.exit(main())
