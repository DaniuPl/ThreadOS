#!/usr/bin/env python3
"""ThreadOS smoke test: Bluetooth - the packets, and an adapter made up.

QEMU emulates no Bluetooth adapter: the whole stack was thrown out of
it years ago. The second half of this test makes one up instead
(scripts/btadapter.py): an HCI controller on USB, plugged in through
usbredir, with a phone and a watch around it, written from the Core
specification and as strict as a real controller about the events the
host's mask leaves out. Against it, 'bt scan' has to find both, by
name: the phone by an inquiry - its name in the extended result, or,
from an adapter too old for those, through a Remote Name Request - and
the watch by an active LE scan, its name being only in its scan
response. The first version of the driver found neither: it did no
inquiry at all, and the LE events were masked off.

The first half is the part that is pure bytes: building commands
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
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, make_iso64
from usbredir import Redir
from btadapter import (BtAdapter, SET_EVENT_MASK, INQUIRY, LE_SCAN_PARAMS,
                       REMOTE_NAME, WRITE_INQUIRY_MODE)


def with_adapter(eir):
    """A machine with the made-up adapter in its xHCI socket: what it
    says about the adapter, and what 'bt scan' finds"""
    kind = "extended inquiry results" if eir else "an adapter from before " \
        "extended inquiry results"
    print("--- an adapter, a phone and a watch (%s) ---" % kind)
    bt = BtAdapter(eir=eir)
    r = Redir(bt)
    work = tempfile.mkdtemp()
    try:
        q = Qemu(KERNEL, INITRD, append=TEXT_MODE,
                 serial=os.path.join(work, "s.log"),
                 devices=["-device", "qemu-xhci,id=xhci", *r.qemu_args(0)])
        try:
            q.wait_serial("thread64>", timeout=90)
            try:
                q.wait_serial("[ OK ] Bluetooth:", timeout=60)
            except Exception:
                pass
            line = next((l for l in q.serial().splitlines()
                         if "Bluetooth:" in l and "[ OK ]" in l), "")
            check("the adapter is found and answers: its address and name "
                  "(%s)" % line.strip(),
                  "00:1b:dc:0f:4e:21, ThreadOS test adapter" in line)
            masks = bt.sent(SET_EVENT_MASK)
            mask = int.from_bytes(masks[-1], "little") if masks else 0
            check("the event mask lets LE events (bit 61) and extended "
                  "inquiry results (bit 46) through (%016x)" % mask,
                  mask >> 61 & 1 and mask >> 46 & 1)

            out = q.command("bt scan 4", "bt: devices heard", timeout=90)
            found = dict(re.findall(r"^\s+([0-9a-f:]{17})\s+(.*?)\s*$", out,
                                    re.M))
            check("the phone is found by its name (%s)"
                  % found.get("58:24:29:a1:b2:c3"),
                  found.get("58:24:29:a1:b2:c3", "").startswith("Pixel 8"))
            check("with its signal strength (from the inquiry result, "
                  "in either mode)", "Pixel 8  -48 dBm" in out)
            check("the watch by the name in its scan response (%s)"
                  % found.get("c0:ff:ee:00:11:22"),
                  found.get("c0:ff:ee:00:11:22", "").startswith(
                      "ThreadOS Watch  -61 dBm"))
            check("two devices, and nothing else", "devices heard: 2" in out)

            inq = bt.sent(INQUIRY)
            check("an inquiry of the general access code (%s)"
                  % [p.hex() for p in inq],
                  inq and inq[-1][:3] == bytes([0x33, 0x8B, 0x9E]))
            le = bt.sent(LE_SCAN_PARAMS)
            check("an active LE scan", le and le[-1][0] == 1)
            modes = [p[0] for p in bt.sent(WRITE_INQUIRY_MODE)]
            names = bt.sent(REMOTE_NAME)
            if eir:
                check("inquiry mode 2, and no name asked for - it came "
                      "with the answer (%s, %d)" % (modes, len(names)),
                      modes == [2] and not names)
            else:
                check("refused mode 2, mode 1; and the name asked for "
                      "(%s, %d)" % (modes, len(names)),
                      modes == [2, 1] and len(names) == 1 and
                      names[0][:6] == bytes.fromhex("c3b2a1292458"))
        finally:
            q.close()
    finally:
        r.close()
        for f in os.listdir(work):
            os.unlink(os.path.join(work, f))
        os.rmdir(work)


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
            # Waits for the last line, so that every line before it is in
            out = q.command("bt test", "not a conversation with an adapter",
                            timeout=30)
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

            # A real adapter's description: the voice interface after
            # the HCI one has the same class, and taking it for the
            # adapter left no event endpoint - every real adapter read
            # as "no adapter".
            check("an Intel 3160's configuration: the HCI interface, not "
                  "the voice one beside it",
                  "Intel 3160 configuration: interface 0, events on "
                  "endpoint 1 (64 bytes), data in 2 / out 2" in out)
            # Events longer than one USB packet, gathered from pieces
            check("an event in sixteen 16-byte pieces read as one, the "
                  "name whole",
                  "name from 16-byte pieces: ThreadOS laptop adapter "
                  "(1 event)" in out)
            check("a transfer cut short is dropped, and the next event "
                  "read clean",
                  "after a cut-off event, the next one read: "
                  "00:1a:7d:da:71:13" in out)

            # A scan with no adapter must refuse, not pretend
            out = q.command("bt scan 1", "bt:", timeout=30)
            check("a scan with no adapter refuses instead of showing an empty list",
                  "there is no adapter" in out
                  and "devices heard" not in out)
        finally:
            q.close()
    finally:
        for path in (iso, slog, disk):
            if os.path.exists(path):
                os.unlink(path)

    with_adapter(eir=True)
    with_adapter(eir=False)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
