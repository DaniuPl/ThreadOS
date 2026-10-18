#!/usr/bin/env python3
"""ThreadOS smoke test: a USB mouse, and a machine with no PS/2 at all.

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
        boot = q.wait_for("ThreadOS shell", timeout=60)
        check("the kernel found the USB mouse and says where ('%s')"
              % (re.search(r"A USB mouse[^\n]*", boot).group(0)
                 if re.search(r"A USB mouse[^\n]*", boot) else "none"),
              re.search(r"A USB mouse \(HID, [^)]+\) on port \d+", boot)
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
        q.wait_for("ThreadOS shell", timeout=60)
        q.type_line("hardware")
        time.sleep(3)
        said = q.screen()
        check("'hardware' talks about the 8042 controller and the PS/2 mouse ('%s')"
              % next((l.strip() for l in said.splitlines()
                      if l.startswith("PS/2:")), "no line"),
              "PS/2:" in said and "the 8042 controller is there" in said)
        check("and about what it found on USB ('%s')"
              % next((l.strip() for l in said.splitlines()
                      if l.startswith("USB:")), "no line"),
              "USB:" in said and "mouse yes" in said)
        check("and it lists the PCI devices, so a foreign machine will describe itself",
              "class" in said and "graphics" in said)
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
        check("a machine with no 8042 comes up and draws the desktop",
              q.wait_desktop(timeout=60))

        # The pointer starts in the middle of the mode the compositor
        # set up; from there it is moved to a place nothing else draws
        # on, and the white cursor has to be there.
        q.mouse_move(-110, -60)
        check("the pointer is where the USB mouse took it (~50,41)",
              q.wait_pixel(50, 41, (255, 255, 255)))

        q.mouse_move(150, 80)
        check("and it keeps following it (~200,121)",
              q.wait_pixel(200, 121, (255, 255, 255)))
        check("and the old place was painted over with the background (~50,41)",
              q.wait_background(50, 41, tol=0))

        # The Windows key on a USB keyboard is a modifier bit in the
        # report, not a key - and it opens the menu all the same
        q.sendkey("meta_l")
        time.sleep(1.5)
        _, _, at = q.screen_raw()
        check("the Windows key on the USB keyboard opens the menu",
              near(at(MENU_X0 + 8, menu_y(MENU_ITEMS[0])),
                   theme("dark")["accent"], 6))
    finally:
        q.close()

    # And such a machine says so rather than claiming a keyboard and a
    # mouse it does not have. Typed on the USB keyboard, because there
    # is no other.
    q = Qemu(KERNEL, INITRD, append=TEXT_MODE, usbkbd="xhci",
             usbmouse="xhci", no_i8042=True)
    try:
        q.wait_for("ThreadOS shell", timeout=60)
        q.type_line("hardware")
        time.sleep(3)
        said = q.screen()
        check("a machine with no 8042 says so outright instead of faking PS/2 ('%s')"
              % next((l.strip() for l in said.splitlines()
                      if l.startswith("PS/2:")), "no line"),
              "NO 8042 controller" in said)
    finally:
        q.close()

    return summary()


if __name__ == "__main__":
    sys.exit(main())
