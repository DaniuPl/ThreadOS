#!/usr/bin/env python3
"""ThreadOS smoke test: Bluetooth - what can be checked without one.

This group is different from every other one in this project, and it
says so in its own output.

QEMU emulates no Bluetooth adapter: the whole stack was thrown out of
it years ago, and no other hypervisor has one either, because a guest
gets its network through a card that looks wired. So the conversation
with an adapter cannot be tested here at all - not badly, not
partially, not at all.

What CAN be tested is the half that is pure bytes: building commands
and reading events. Recorded packets of the shape the specification
gives go through the same parser the transport uses, and what comes out
has to be the address, the name, the version and the devices that were
in those bytes. A parser that reads an address backwards, or takes the
manufacturer from one byte too early, fails here - and those are
exactly the mistakes that would otherwise be found on a laptop, one
reboot at a time.

The second thing this checks is honesty: on a machine with no adapter,
the system has to SAY there is no adapter, and say why, rather than
show an empty list of devices as though it had looked.

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, make_iso64


def main():
    print()
    print("--- Bluetooth (packets, because there is nothing to fake an adapter with) ---")

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

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, usbkbd="xhci", qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=90)

            # --- the machine has no adapter, and has to say so --------
            out = q.command("bt", "bt:", timeout=20)
            check("the system does not pretend to have Bluetooth",
                  "there is no adapter" in out)
            check("and says why there is none",
                  "Bluetooth adapter on the USB bus" in out
                  or "there is no USB controller" in out)

            said = q.command("hardware", "PCI:", timeout=30)
            check("'hardware' lists Bluetooth too among what is missing",
                  "Bluetooth: none" in said)

            # --- the packets ------------------------------------------
            out = q.command("bt test", "heard", timeout=30)
            check("the adapter address read out of the packet the right way round",
                  "adapter address: 00:1a:7d:da:71:13" in out)
            check("the adapter name pulled out of the answer",
                  "adapter name: Thread" in out)
            check("the version and the maker from Read Local Version",
                  "version: Bluetooth 5.0" in out and "000f" in out)
            check("ACL headers preserve handle, flags and L2CAP payload",
                  "ACL framing: OK" in out)
            check("two devices out of two different kinds of event",
                  "devices heard: 2" in out)
            check("an LE device with a name from its advertising and a signal strength",
                  "cb:a9:87:65:43:21" in out and "Headphones" in out
                  and "-59 dBm" in out)
            check("a classic device from an inquiry result",
                  "66:55:44:33:22:11" in out)
            check("and the test says outright what it does NOT check",
                  "not a conversation with an adapter" in out)

            # A scan with no adapter must refuse, not pretend
            out = q.command("bt scan 1", "bt:", timeout=30)
            check("a scan with no adapter refuses instead of showing an empty list",
                  "there is no adapter" in out
                  and "devices heard" not in out)
        finally:
            q.close()

        # The same code compiled for the other mode - 'hardware' says the
        # same thing there, because it is the same file.
        disk32 = tempfile.mktemp(suffix=".img")
        with open(disk32, "wb") as fh:
            fh.truncate(DISK_SIZE)
        q = Qemu(KERNEL, INITRD, disk32, append=TEXT_MODE,
                 serial=tempfile.mktemp(suffix=".log"))
        try:
            q.wait_for("ThreadOS shell")
            q.type_line("hardware")
            screen = q.wait_for("Bluetooth:", timeout=30)
            check("in 32-bit mode the same line about Bluetooth",
                  "Bluetooth: none" in screen)
        finally:
            q.close()
            os.unlink(disk32)
    finally:
        for path in (iso, slog, disk):
            if os.path.exists(path):
                os.unlink(path)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
