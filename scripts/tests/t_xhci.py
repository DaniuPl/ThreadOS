#!/usr/bin/env python3
"""ProtonOS smoke test: a USB keyboard on an xHCI controller.

xHCI is the third and last USB controller - the only one newer boards
have. It differs from UHCI not in details but in the division of labour:
there the driver laid transfer descriptors out in a frame list and
assigned addresses to devices itself, here it puts commands into a ring
and collects the answers through a second ring - of events.

We check two things at once:

  1. that the driver brings the keyboard to a conversation - a port
     reset, a slot assignment, an address, reading the configuration, the
     boot protocol, and then HID reports travelling through the transfer
     ring;
  2. that HIGHER UP no difference can be seen. The same group of checks
     as with UHCI (t_usbkbd64.py), on the same keyboard and the same HID
     code - the only difference is the bus underneath. The usbhc.c layer
     is to hide that, and that is exactly what we measure here.

We do it in both kernels: the controller addresses memory with 64 bits,
so in 32-bit mode the upper halves of the addresses are zeros, and the
64-bit registers are written in two halves. If something in that is
wrong, only one of the kernels will start.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
LOADER = os.path.join(ROOT, "build", "BOOTX64.EFI")


def type_text(q, text, delay=0.12):
    for ch in text:
        q.sendkey(ch)
        time.sleep(delay)
    q.sendkey("ret")
    time.sleep(0.25)


def main():
    print("--- a USB keyboard on xHCI (the 64-bit kernel) ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
    else:
        d = tempfile.mkdtemp()
        iso = tempfile.mktemp(suffix=".iso")
        slog = tempfile.mktemp(suffix=".log")
        try:
            check("the disc with the 64-bit kernel builds", make_iso64(d, iso))
            if not os.path.exists(iso):
                return summary()

            q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, usbkbd="xhci",
                     qemu="qemu-system-x86_64")
            try:
                start = q.wait_serial("A USB keyboard", timeout=60)
                # The selection layer says by itself what it is talking
                # through - and it is to say xHCI here, because there is
                # no UHCI on this machine
                check("the keyboard was found on the xHCI controller",
                      re.search(r"A USB keyboard \(HID, xHCI\) on port \d+",
                                start) is not None)

                q.wait_serial("proton64>", timeout=30)
                time.sleep(0.5)
                before = len(q.serial())
                type_text(q, "uptime")
                time.sleep(1.5)
                po = q.serial()[before:]
                check("a command typed through xHCI runs",
                      "Uptime:" in po)

                # A capital letter comes from a modifier in the HID report -
                # that is a different path through a report than an
                # ordinary key code. QEMU holds a key for about a hundred
                # milliseconds, so with too fast a rate neighbouring keys
                # overlap; hence the slower pace and a second attempt.
                big_one = False
                for _ in range(3):
                    before = len(q.serial())
                    for ch in "echo":
                        q.sendkey(ch)
                        time.sleep(0.15)
                    for k in ("spc", "shift-a", "ret"):
                        q.sendkey(k)
                        time.sleep(0.25)
                    time.sleep(1.0)
                    if "echo A" in q.serial()[before:]:
                        big_one = True
                        break
                check("the Shift modifier from the HID report gives a capital letter",
                      big_one)

                # Alt+F2 is a different path inside the kernel than an
                # ordinary character (a console switch, not the input
                # queue)
                q.sendkey("alt-f2")
                time.sleep(1.0)
                check("Alt+F2 from the xHCI keyboard switches consoles",
                      "console 2" in q.screen_fbcon())
            finally:
                q.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)
            if os.path.exists(iso):
                os.unlink(iso)
            if os.path.exists(slog):
                os.unlink(slog)

    # --- A hub between the machine and the keyboard -----------------
    #
    # What a docking station looks like from the machine's side, and a
    # thing that was simply invisible until hubs were understood: a
    # device behind a hub is not on any port of the controller, so
    # walking the controller's ports finds nothing. The hub's own ports
    # have to be asked about, given power and reset one by one - and
    # then the controller has to be told the WAY to what was found,
    # four bits per tier of hubs.
    if not no64():
        print("--- a keyboard behind a USB hub ---")
        dh = tempfile.mkdtemp()
        isoh = tempfile.mktemp(suffix=".iso")
        slogh = tempfile.mktemp(suffix=".log")
        try:
            check("the disc for the hub attempt builds",
                  make_iso64(dh, isoh, text=True))
            qh = Qemu(KERNEL, INITRD, cdrom=isoh, serial=slogh,
                      usbkbd="hub", qemu="qemu-system-x86_64")
            try:
                start = qh.wait_serial("proton64>", timeout=90)
                check("the system finds the hub and counts its sockets",
                      re.search(r"A USB hub in socket \d+: "
                                r"\d+ sockets more", start) is not None)

                m = re.search(r"A USB keyboard \(HID, xHCI\) on port "
                              r"(\d+)", start)
                check("and the keyboard BEHIND it - in a socket the "
                      "there is no controller"
                      + (" (port %s)" % m.group(1) if m else ""),
                      m is not None and int(m.group(1)) > 4)

                # And it is a keyboard, not a line in a log: what is
                # typed on it has to reach the shell.
                before = len(qh.serial())
                for ch in "uptime":
                    qh.sendkey(ch)
                    time.sleep(0.12)
                qh.sendkey("ret")
                time.sleep(2)
                typed = qh.serial()[before:]
                check("and what is typed on it reaches the shell",
                      "Uptime:" in typed)
            finally:
                qh.close()
        finally:
            shutil.rmtree(dh, ignore_errors=True)
            for f in (isoh, slogh):
                if os.path.exists(f):
                    os.unlink(f)

    # --- And the same machine started through UEFI ------------------
    #
    # This is where a real laptop starts, and it turned out to be a
    # different machine from the one booted off a disc: UEFI firmware
    # gives the controller a memory window ABOVE 4 GiB, and a driver
    # that refuses such an address has no USB at all - so no keyboard
    # and no mouse on a machine that has nothing else. Nothing caught
    # it, because every USB test until now booted through the old BIOS.
    reason = no64()
    if not reason and os.path.exists(OVMF_CODE) and os.path.exists(OVMF_VARS):
        print("--- a USB keyboard on xHCI, booted through UEFI ---")
        esp = tempfile.mkdtemp()
        nvram = tempfile.mktemp(suffix=".fd")
        slogu = tempfile.mktemp(suffix=".log")
        try:
            if not os.path.exists(LOADER):
                subprocess.run(["make", "efi"], cwd=ROOT,
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
            if not os.path.exists(LOADER):
                print("SKIPPED: no build/BOOTX64.EFI")
            else:
                os.makedirs(os.path.join(esp, "EFI", "BOOT"))
                shutil.copy(LOADER,
                            os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI"))
                shutil.copy(KERNEL64, os.path.join(esp, "protonos64.bin"))
                shutil.copy(INITRD64, os.path.join(esp, "initrd64.img"))
                shutil.copy(OVMF_VARS, nvram)

                q4 = Qemu(KERNEL, INITRD, esp=esp, serial=slogu,
                          usbkbd="xhci", qemu="qemu-system-x86_64",
                          uefi=(OVMF_CODE, nvram))
                try:
                    start = q4.wait_serial("The 64-bit system is ready",
                                           timeout=140)
                    check("after a UEFI boot the USB keyboard is "
                          "finds",
                          re.search(r"A USB keyboard \(HID, xHCI\)",
                                    start) is not None)
                    said = q4.command("hardware", "PCI:", timeout=30)
                    check("and 'hardware' no longer says 'USB: none'",
                          "USB:      xHCI" in said)
                    check("and if it did, it would say why",
                          "USB:      none" not in said or "(" in said)
                finally:
                    q4.close()
        finally:
            shutil.rmtree(esp, ignore_errors=True)
            for f in (nvram, slogu):
                if os.path.exists(f):
                    os.unlink(f)

    # --- The same in the 32-bit kernel ---
    print("--- a USB keyboard on xHCI (the 32-bit kernel) ---")
    slog32 = tempfile.mktemp(suffix=".log")
    try:
        q3 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog32,
                  usbkbd="xhci")
        try:
            start = q3.wait_serial("A USB keyboard", timeout=60)
            check("the keyboard was found on the xHCI controller (32-bit)",
                  re.search(r"A USB keyboard \(HID, xHCI\) on port \d+",
                            start) is not None)
            check("the controller did not report itself as UHCI",
                  "HID, UHCI" not in start)

            q3.wait_serial("ProtonOS shell", timeout=40)
            time.sleep(1.0)
            before = len(q3.serial())
            type_text(q3, "uptime")
            time.sleep(1.5)
            po = q3.serial()[before:]
            check("a command typed through xHCI runs (32-bit)",
                  "uptime:" in po and "ticks" in po)

            # Several keys in a row: the reports have to go one after another
            # through the same transfer ring, not just the first one
            before = len(q3.serial())
            type_text(q3, "help")
            time.sleep(1.5)
            help_text = q3.serial()[before:]
            check("the whole word arrives character by character (32-bit)",
                  "help" in help_text and "Available commands" in help_text)
        finally:
            q3.close()
    finally:
        if os.path.exists(slog32):
            os.unlink(slog32)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
