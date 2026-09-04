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
  with the 8042 switched OFF - a machine that genuinely has no PS/2,
  and so no way to TYPE anything either - the pointer on the desktop
  still follows the mouse, which it can only do through USB

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

    # --- the machine says what it found ------------------------------
    # Written for a first boot on hardware nobody has tried yet: a
    # laptop whose pointer does not move is either a laptop with
    # nothing on the PS/2 port or one whose touchpad is on a bus we do
    # not speak, and only the machine can say which.
    q = Qemu(KERNEL, INITRD, append=TEXT_MODE, usbkbd="xhci",
             usbmouse="xhci")
    try:
        q.wait_for("ProtonOS shell", timeout=60)
        q.type_line("sprzet")
        time.sleep(3)
        said = q.screen()
        check("'sprzet' mowi o kontrolerze 8042 i myszy PS/2 ('%s')"
              % next((l.strip() for l in said.splitlines()
                      if l.startswith("PS/2:")), "brak wiersza"),
              "PS/2:" in said and "kontroler 8042 jest" in said)
        check("i o tym, co znalazl na USB ('%s')"
              % next((l.strip() for l in said.splitlines()
                      if l.startswith("USB:")), "brak wiersza"),
              "USB:" in said and "mysz tak" in said)
        check("i wymienia urzadzenia PCI, wiec obca maszyna sie opisze",
              "klasa" in said and "grafika" in said)
    finally:
        q.close()

    # --- and now a machine that has no PS/2 at all -------------------
    # Without the 8042 there is no PS/2 keyboard either, so nothing can
    # be TYPED here - the question has to be asked in pixels. The
    # desktop draws the pointer where the pointer is, so moving the
    # mouse and looking at the screen says everything: whatever moved
    # that cursor came over USB, because there is no other road.
    q = Qemu(KERNEL, INITRD, usbkbd="xhci", usbmouse="xhci",
             no_i8042=True)
    try:
        check("maszyna bez 8042 wstaje i rysuje pulpit",
              q.wait_desktop(timeout=60))

        # The pointer starts in the middle of the mode the compositor
        # set up; from there it is moved to a place nothing else draws
        # on, and the white cursor has to be there.
        q.mouse_move(-110, -60)
        check("kursor jest tam, gdzie mysz USB go zaprowadzila (~50,41)",
              q.wait_pixel(50, 41, (255, 255, 255)))

        q.mouse_move(150, 80)
        check("i idzie za nia dalej (~200,121)",
              q.wait_pixel(200, 121, (255, 255, 255)))
        check("a stare miejsce zostalo zamalowane tlem (~50,41)",
              q.wait_pixel(50, 41, (0, 128, 160)))
    finally:
        q.close()

    # And such a machine says so rather than claiming a keyboard and a
    # mouse it does not have. Typed on the USB keyboard, because there
    # is no other.
    q = Qemu(KERNEL, INITRD, append=TEXT_MODE, usbkbd="xhci",
             usbmouse="xhci", no_i8042=True)
    try:
        q.wait_for("ProtonOS shell", timeout=60)
        q.type_line("sprzet")
        time.sleep(3)
        said = q.screen()
        check("maszyna bez 8042 mowi to wprost, zamiast udawac PS/2 ('%s')"
              % next((l.strip() for l in said.splitlines()
                      if l.startswith("PS/2:")), "brak wiersza"),
              "BRAK kontrolera 8042" in said)
    finally:
        q.close()

    return summary()


if __name__ == "__main__":
    sys.exit(main())
