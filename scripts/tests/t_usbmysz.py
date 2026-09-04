#!/usr/bin/env python3
"""ProtonOS smoke test: a USB mouse, and a machine with no PS/2 at all.

The pointer arrived through the 8042 controller from the first day, and
on a desktop machine it still does. A laptop made in the last few years
may have no 8042: no PS/2 keyboard, no PS/2 touchpad, nothing on those
ports because the ports are not there. Its pointer is a HID device on
the USB bus, and so is its keyboard.

So the mouse driver next to the keyboard one, walking the same road -
the ports the controller layer reports, the configuration descriptor,
an interface of the HID class in its boot version - and parting from it
on one byte: the interface protocol is 1 for a keyboard and 2 for a
mouse.

Two questions, and the second is the one that matters:

  the machine finds the mouse on the xHCI controller and says so
  with the 8042 switched OFF - a machine that genuinely has no PS/2 -
  the pointer still moves, which it can only do through USB

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary


def main():
    # --- a mouse on the bus every newer board has --------------------
    q = Qemu(KERNEL, INITRD, append=TEXT_MODE, usbkbd="xhci",
             usbmouse="xhci")
    try:
        boot = q.wait_for("ProtonOS shell", timeout=60)
        check("jadro znalazlo mysz USB i mowi, gdzie ('%s')"
              % (re.search(r"Mysz USB[^\n]*", boot).group(0)
                 if re.search(r"Mysz USB[^\n]*", boot) else "brak"),
              re.search(r"Mysz USB \(HID, [^)]+\) na porcie \d+", boot)
              is not None)
    finally:
        q.close()

    # --- and now a machine that has no PS/2 at all -------------------
    # Without the 8042 the old driver finds nothing: no port, no
    # packets, no pointer. Whatever moves the pointer here came over
    # USB, and there is no other reading of it.
    q = Qemu(KERNEL, INITRD, append=TEXT_MODE, usbkbd="xhci",
             usbmouse="xhci", no_i8042=True)
    try:
        q.wait_for("ProtonOS shell", timeout=60)

        q.type_line("mouse")
        start = q.wait_for("mysz: x=")
        m = re.search(r"mysz: x=(\d+) y=(\d+)", start)
        check("maszyna bez 8042 wstaje i ma wskaznik ('%s')"
              % (m.group(0) if m else start[-40:]), m is not None)
        if not m:
            return summary()
        x0, y0 = int(m.group(1)), int(m.group(2))

        q.mouse_move(40, 30)
        time.sleep(1)
        q.type_line("mouse")
        after = q.wait_for("mysz: x=")
        m2 = None
        for m2 in re.finditer(r"mysz: x=(\d+) y=(\d+)", after):
            pass
        x1, y1 = int(m2.group(1)), int(m2.group(2))
        check("i wskaznik idzie za mysza po USB (%d,%d -> %d,%d)"
              % (x0, y0, x1, y1), x1 > x0 and y1 > y0)
    finally:
        q.close()

    return summary()


if __name__ == "__main__":
    sys.exit(main())
