#!/usr/bin/env python3
"""ThreadOS smoke test: a USB keyboard in the 64-bit kernel.

A machine newer than roughly 2010 no longer has a PS/2 connector. This
group looks after the older bus - UHCI. The newer one, xHCI, has its own
(t_xhci.py); what matters is that HIGHER UP no difference between them
can be seen.

The UHCI driver and the HID class in the boot protocol are the same
files as in the 32-bit kernel - in long mode the only thing needed was
explicit casts of the structures' addresses, because the controller
addresses memory with 32 bits while a pointer here is eight bytes wide.

QEMU is given a 'usb-kbd' and directs keys to it, so the mere fact that
a command runs is proof that they went the whole way: through the
controller's descriptor queue, an HID report and the console's queue.
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


def main():
    print("--- a USB keyboard (HID) in the 64-bit kernel ---")

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
                time.sleep(1.0)
                if "echo A" in q.serial()[before:]:
                    big_one = True
                    break
            check("the Shift modifier from the HID report gives a capital letter", big_one)

            # Alt+F2 from a USB keyboard switches the console - that is a
            # different path inside the kernel than an ordinary character
            # (a switch, not the queue)
            q.sendkey("alt-f2")
            time.sleep(1.0)
            second_one = q.screen_fbcon()
            check("Alt+F2 from the USB keyboard switches consoles",
                  "console 2" in second_one)
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
