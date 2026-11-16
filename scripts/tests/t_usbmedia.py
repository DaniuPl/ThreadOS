#!/usr/bin/env python3
"""ThreadOS smoke test: media keys on USB, read from the device's own
report descriptor.

A keyboard's loudness keys rarely come with its letters: most send them
from a second HID interface of the same device, in a report the device
lays out itself, and a volume knob is nothing but such an interface.
The kernel now reads report descriptors (kernel/hid.c), binds every
HID interface with keys in it (kernel/usbkbd.c), and reads several
interrupt endpoints of one device at once (kernel/xhci.c).

QEMU has no such device, so the test makes them (scripts/usbredir.py)
and plugs them in through QEMU's usb-redir: a keyboard whose second
interface sends the consumer page as a 16-bit array under report id 2
and system keys under id 1 - the layout most keyboards have - and a
volume knob that sends one bit per key with no report id. Every key is
checked twice: the island at the top of the screen says the loudness,
and the driver (the shell's 'volume', on the serial port) agrees.

On the way: a system-key report while Volume Up is held, and the held
key sent again after it, are not a second press; the keyboard's own
interface still works (its Volume Up on the keyboard page); reports
from both interfaces of one device sent together both arrive; the
kernel asked each described interface for its report descriptor and
put only the keyboard interface in the boot protocol.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64
from usbredir import (Redir, HidDevice, HidInterface, BOOT_KEYBOARD,
                      usage_page, usage, usage_min, usage_max, logical_min,
                      logical_max, report_size, report_count, report_id,
                      collection, end_collection, input_)

MEDIA_ARRAY = (
    usage_page(0x0C) + usage(0x01) + collection(1) +
    report_id(2) + logical_min(0) + logical_max(0x23C) + usage_min(0) +
    usage_max(0x23C) + report_size(16) + report_count(1) + input_(0x00) +
    end_collection() +
    usage_page(0x01) + usage(0x80) + collection(1) +
    report_id(1) + usage_min(0x81) + usage_max(0x83) + logical_min(0) +
    logical_max(1) + report_size(1) + report_count(3) + input_(0x02) +
    report_count(5) + input_(0x01) + end_collection())

KNOB_BITS = (
    usage_page(0x0C) + usage(0x01) + collection(1) +
    logical_min(0) + logical_max(1) + report_size(1) + report_count(8) +
    usage(0xE9) + usage(0xEA) + usage(0xE2) + usage(0xCD) + usage(0xB5) +
    usage(0xB6) + usage(0xB7) + usage(0xB0) + input_(0x02) +
    end_collection())

UP, DOWN, MUTE = 0xE9, 0xEA, 0xE2


def kernel_volume(q):
    out = q.command("volume", "steps in the codec", timeout=20)
    m = re.search(r"volume: (\d+)%( \(muted\))?, \d+ steps", out)
    return (int(m.group(1)), bool(m.group(2))) if m else (None, None)


def wait_for(fn, want, timeout=10):
    deadline = time.time() + timeout
    got = fn()
    while got != want and time.time() < deadline:
        time.sleep(0.3)
        got = fn()
    return got


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    kbd = HidDevice([HidInterface(1, 1, BOOT_KEYBOARD, ep=1),
                     HidInterface(0, 0, MEDIA_ARRAY, ep=2)],
                    product=0x0001, name="keyboard with media keys")
    knob = HidDevice([HidInterface(0, 0, KNOB_BITS, ep=1)],
                     product=0x0002, name="volume knob")
    ra, rb = Redir(kbd), Redir(knob)
    work = tempfile.mkdtemp()
    try:
        q = Qemu(KERNEL, INITRD, serial=os.path.join(work, "s.log"),
                 audio=os.path.join(work, "a.wav"), qemu="qemu-system-x86_64",
                 devices=["-device", "qemu-xhci,id=xhci",
                          *ra.qemu_args(0), *rb.qemu_args(1)])
        try:
            q.wait_desktop()
            log = q.serial()
            check("the keyboard's keys: its boot interface",
                  re.search(r"A USB keyboard \(HID, xHCI\) on port \d+, "
                            r"endpoint 1", log))
            check("its second interface: media keys, two fields (the "
                  "consumer array and the system keys)",
                  re.search(r"USB media keys \(HID report of 2 fields\) on "
                            r"port \d+, interface 1, endpoint 2", log))
            check("the knob: media keys, one field of eight buttons",
                  re.search(r"USB media keys \(HID report of 1 field\) on "
                            r"port \d+, interface 0, endpoint 1", log))
            # (the firmware enumerates the keyboard too, and first: what
            # counts is what came after the kernel reset the device)
            asked = lambda d, i: (0x81, 6, 0x2200, i) in d.since_reset()
            check("the kernel read the report descriptors of the described "
                  "interfaces, and not the boot keyboard's",
                  asked(kbd, 1) and asked(knob, 0) and not asked(kbd, 0))
            boot = [r for r in kbd.since_reset() if r[:2] == (0x21, 0x0B)]
            check(f"and put only the keyboard interface in the boot protocol "
                  f"({boot})", boot == [(0x21, 0x0B, 0, 0)])
            deadline = time.time() + 20
            while not (ra.reading(1) and ra.reading(2) and rb.reading(1)) \
                    and time.time() < deadline:
                time.sleep(0.3)
            check("all three interrupt endpoints are being read",
                  ra.reading(1) and ra.reading(2) and rb.reading(1))
            wait_for(lambda: island_volume(q), "", 10)

            def step(what, want_text, want_kernel):
                got = wait_for(lambda: island_volume(q), want_text)
                k = kernel_volume(q)
                check(f"{what}: the island says {want_text} ('{got}'), the "
                      f"driver {k}", got == want_text and k == want_kernel)

            ra.press(2, [2, UP, 0], [2, 0, 0])
            step("the keyboard's media Volume Up", "85%", (85, False))
            ra.press(2, [2, DOWN, 0], [2, 0, 0])
            step("its Volume Down", "80%", (80, False))

            # Held, a system report in between, the held key again
            ra.report(2, [2, UP, 0])
            time.sleep(0.2)
            ra.report(2, [1, 0])
            time.sleep(0.2)
            ra.report(2, [2, UP, 0])
            time.sleep(0.2)
            ra.report(2, [2, 0, 0])
            step("Volume Up held across a system-key report is one press",
                 "85%", (85, False))

            ra.press(1, [0, 0, 0x80, 0, 0, 0, 0, 0], [0] * 8)
            step("the keyboard interface's own Volume Up (keyboard page)",
                 "90%", (90, False))

            # Both interfaces of one device at once
            ra.report(1, [0, 0, 0x81, 0, 0, 0, 0, 0])
            ra.report(2, [2, DOWN, 0])
            time.sleep(0.2)
            ra.report(1, [0] * 8)
            ra.report(2, [2, 0, 0])
            step("Volume Down from both interfaces at once: both arrive",
                 "80%", (80, False))

            ra.press(2, [2, MUTE, 0], [2, 0, 0])
            step("the keyboard's Mute", "Off", (80, True))
            ra.press(2, [2, MUTE, 0], [2, 0, 0])
            step("Mute again", "80%", (80, False))

            rb.press(1, [0x01], [0x00])
            step("the knob's Volume Up bit", "85%", (85, False))
            rb.press(1, [0x02], [0x00])
            step("its Volume Down bit", "80%", (80, False))
            rb.press(1, [0x04], [0x00])
            step("its Mute bit", "Off", (80, True))
            rb.press(1, [0x04], [0x00])
            step("and again", "80%", (80, False))
        finally:
            q.close()
    finally:
        ra.close()
        rb.close()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
