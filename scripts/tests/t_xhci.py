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
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def type_text(q, text, delay=0.12):
    for ch in text:
        q.sendkey(ch)
        time.sleep(delay)
    q.sendkey("ret")
    time.sleep(0.25)


def main():
    print("--- klawiatura USB na xHCI (jadro 64-bitowe) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
    else:
        d = tempfile.mkdtemp()
        iso = tempfile.mktemp(suffix=".iso")
        slog = tempfile.mktemp(suffix=".log")
        try:
            check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
            if not os.path.exists(iso):
                return summary()

            q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, usbkbd="xhci",
                     qemu="qemu-system-x86_64")
            try:
                start = q.wait_serial("Klawiatura USB", timeout=60)
                # The selection layer says by itself what it is talking
                # through - and it is to say xHCI here, because there is
                # no UHCI on this machine
                check("klawiatura znaleziona na kontrolerze xHCI",
                      re.search(r"Klawiatura USB \(HID, xHCI\) na porcie \d+",
                                start) is not None)

                q.wait_serial("proton64>", timeout=30)
                time.sleep(0.5)
                before = len(q.serial())
                type_text(q, "uptime")
                time.sleep(1.5)
                po = q.serial()[before:]
                check("polecenie wpisane przez xHCI wykonuje sie",
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
                check("modyfikator Shift z raportu HID daje wielka litere",
                      big_one)

                # Alt+F2 is a different path inside the kernel than an
                # ordinary character (a console switch, not the input
                # queue)
                q.sendkey("alt-f2")
                time.sleep(1.0)
                check("Alt+F2 z klawiatury na xHCI przelacza konsole",
                      "konsola 2" in q.screen_fbcon())
            finally:
                q.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)
            if os.path.exists(iso):
                os.unlink(iso)
            if os.path.exists(slog):
                os.unlink(slog)

    # --- The same in the 32-bit kernel ---
    print("--- klawiatura USB na xHCI (jadro 32-bitowe) ---")
    slog32 = tempfile.mktemp(suffix=".log")
    try:
        q3 = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog32,
                  usbkbd="xhci")
        try:
            start = q3.wait_serial("Klawiatura USB", timeout=60)
            check("klawiatura znaleziona na kontrolerze xHCI (32-bit)",
                  re.search(r"Klawiatura USB \(HID, xHCI\) na porcie \d+",
                            start) is not None)
            check("kontroler nie zglosil sie jako UHCI",
                  "HID, UHCI" not in start)

            q3.wait_serial("ProtonOS shell", timeout=40)
            time.sleep(1.0)
            before = len(q3.serial())
            type_text(q3, "uptime")
            time.sleep(1.5)
            po = q3.serial()[before:]
            check("polecenie wpisane przez xHCI wykonuje sie (32-bit)",
                  "uptime:" in po and "tyknac" in po)

            # Several keys in a row: the reports have to go one after another
            # through the same transfer ring, not just the first one
            before = len(q3.serial())
            type_text(q3, "help")
            time.sleep(1.5)
            help_text = q3.serial()[before:]
            check("cale slowo dochodzi znak po znaku (32-bit)",
                  "help" in help_text and "Dostepne komendy" in help_text)
        finally:
            q3.close()
    finally:
        if os.path.exists(slog32):
            os.unlink(slog32)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
