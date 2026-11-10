#!/usr/bin/env python3
"""ThreadOS smoke test: a USB keyboard (HID).

A machine newer than roughly 2010 no longer has a PS/2 connector. The
first machine here has its keyboard on the older bus - UHCI; the second
on the newer one, xHCI, which also has a group of its own (t_xhci.py).
What matters is that HIGHER UP no difference between them can be seen.

The controller addresses memory with 32 bits while a pointer here is
eight bytes wide, so the UHCI driver and the HID class in the boot
protocol cast the structures' addresses explicitly.

QEMU is given a 'usb-kbd' and directs keys to it, so the mere fact that
a command runs is proof that they went the whole way: through the
controller's descriptor queue, an HID report and the console's queue.
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


def main():
    print("--- a USB keyboard (HID) ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    try:
        check("the disc with the 64-bit kernel builds", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, usbkbd=True,
                 qemu="qemu-system-x86_64")
        try:
            start = q.wait_serial("A USB keyboard", timeout=60)
            check("the USB keyboard was recognised on the bus",
                  re.search(r"A USB keyboard \(HID, UHCI\) on port \d+",
                            start) is not None)

            q.wait_serial("thread64>", timeout=30)
            q.type_line("uptime")
            out_text = q.wait_serial("Uptime:", timeout=20)
            check("a command typed on the USB keyboard runs",
                  "ticks of" in out_text)

            # A capital letter comes from a modifier in the HID report. We
            # poll the keyboard fifty times a second, and QEMU holds a key
            # for about a hundred milliseconds - with too fast a rate the
            # press and release of neighbouring keys overlap. Hence the
            # slower pace and a second attempt.
            def type_shifted():
                for ch in "echo":
                    q.sendkey(ch)
                    time.sleep(0.15)
                for k in ("spc", "shift-a", "ret"):
                    q.sendkey(k)
                    time.sleep(0.25)

            big_one = False
            for _ in range(3):
                before = len(q.serial())
                type_shifted()
                deadline = time.time() + 5
                while time.time() < deadline:
                    if "echo A" in q.serial()[before:]:
                        big_one = True
                        break
                    time.sleep(0.3)
                if big_one:
                    break
            check("the Shift modifier from the HID report gives a capital letter", big_one)

            # Alt+F2 from a USB keyboard switches the console - that is a
            # different path inside the kernel than an ordinary character
            # (a switch, not the queue)
            q.sendkey("alt-f2")
            second_one = q.wait_fbcon("console 2", timeout=15)
            check("Alt+F2 from the USB keyboard switches consoles",
                  "console 2" in second_one)
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog):
            if os.path.exists(f):
                os.unlink(f)

    # --- Fast typing, behind xHCI ---------------------------------------
    # A read on an xHCI (or EHCI) endpoint is two calls: one hands the
    # controller a read to wait in, the next takes what came back. The
    # driver took one report a round, so a read was out only every
    # other round - and two reports to a key (pressed, let go). Typed
    # faster than that, keys queued in the keyboard until it threw them
    # away. Eighty a second, each held ten milliseconds: every letter has
    # to arrive, in order - the shell says back what it was given, as a
    # command it does not know.
    print("--- fast typing on a keyboard behind xHCI ---")
    q = Qemu(KERNEL, INITRD, append=TEXT_MODE, usbkbd="xhci")
    try:
        start = q.wait_serial("A USB keyboard", timeout=40)
        check("the keyboard on xHCI was recognised on the bus",
              re.search(r"A USB keyboard \(HID, xHCI\) on port \d+",
                        start) is not None)
        q.wait_serial("thread64>", timeout=40)
        # An empty line first, and its prompt back: the shell is then
        # reading, and the word below is the only thing on its line
        q.command("", "thread64>", timeout=20)
        word = "thequickbrownfoxjumps"
        before = len(q.serial())
        for ch in word:
            q.hmp("sendkey %s 10" % ch)
            time.sleep(0.012)
        # Enter only once the shell has had time to echo what arrived;
        # a key that was lost never comes, so this is a wait with a
        # limit, not a condition - the check below says what came
        deadline = time.time() + 10
        while time.time() < deadline and word not in q.serial()[before:]:
            time.sleep(0.3)
        said_all = q.command("", "(try 'help')", timeout=20)
        said = [ln.strip() for ln in said_all.splitlines()
                if "Unknown command:" in ln]
        check("eighty keys a second all arrive, in order (%s)"
              % (said[-1] if said else "-"),
              bool(said)
              and "Unknown command: %s (try 'help')" % word in said[-1])
    finally:
        q.close()

    return summary()


if __name__ == "__main__":
    sys.exit(main())
