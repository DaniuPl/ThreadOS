#!/usr/bin/env python3
"""ThreadOS smoke test: a USB keyboard (HID).

The USB controller, device enumeration and the HID boot protocol. QEMU
directs keys to the USB device, so the mere fact that a command runs at
all is proof that they went that whole way.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary


def main():
    # --- The USB keyboard ------------------------------------------
    # The USB controller, device enumeration, the HID boot protocol.
    # QEMU directs keys to the USB device, so the mere fact that a
    # command runs at all is proof that they went that way.
    print("--- a USB keyboard (HID) ---")
    udisk = tempfile.mktemp(suffix=".img")
    with open(udisk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    q19 = Qemu(KERNEL, INITRD, udisk, append=TEXT_MODE, usbkbd=True)
    try:
        boot = q19.wait_for("A USB keyboard", timeout=30)
        check("the USB keyboard was recognised on the bus",
              re.search(r"A USB keyboard \(HID, UHCI\) on port \d+", boot)
              is not None)
        # Waiting, not asserting: with four machines booting at once the
        # shell's greeting can take twice as long to reach the screen as
        # it does on an idle one, and a wait that runs out proves
        # nothing about the keyboard.
        q19.wait_for("ThreadOS shell", timeout=40)
        q19.type_line("clear")
        q19.type_line("uptime")
        out = q19.wait_for("timer ticks", timeout=20)
        check("a command typed on the USB keyboard runs",
              "uptime:" in out)
        # A capital letter comes from a modifier in the HID report -
        # 'type_line' knows only lower case, so we assemble the keys by
        # hand. We poll the keyboard fifty times a second, and QEMU holds
        # a key for about a hundred milliseconds - with too fast a rate
        # the press and release of neighbouring keys overlap. Hence the
        # slower pace and a second attempt.
        def type_shift_a():
            for ch in "write":
                q19.sendkey(ch)
                time.sleep(0.15)
            for k in ("spc", "shift-a", "spc", "x", "ret"):
                q19.sendkey(k)
                time.sleep(0.25)

        saved = ""
        for attempt in range(3):
            type_shift_a()
            try:
                saved = q19.wait_for("wrote", timeout=10)
                if "to 'A'" in saved:
                    break
            except RuntimeError:
                pass
            q19.type_line("clear")
        check("the Shift modifier from the HID report gives capital letters",
              "to 'A'" in saved)
    finally:
        q19.close()
        if os.path.exists(udisk):
            os.unlink(udisk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
