#!/usr/bin/env python3
"""ThreadOS smoke test: the boot, up to the shell.

It asks about what happens BEFORE anybody types anything: did the kernel
reach the end of initialization, did it recognize the hardware, did the
timer, the interrupts and the network start - and then whether the
first keys typed arrive.

k64core asks about the long mode itself (the page tables, the heap, the
threads); this group is about the rest of the road, on a machine in text
mode with the old IDE disk and nothing configured, the way a first boot
looks.

We read the order of the messages from the serial port rather than from
the screen - GRUB hands the kernel a framebuffer even in text mode, and
on a screen the oldest lines scroll away anyway.

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
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    slog = tempfile.mktemp(suffix=".log")

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
    try:
        log = q.wait_serial("The 64-bit system is ready", timeout=90)
        check("the kernel reached the end of its initialization",
              "The 64-bit system is ready - type 'help'" in log)

        # The steps in the order they have to come: each needs the one
        # before it
        steps = ["Multiboot OK", "Long mode: LMA=1", "Physical memory",
                 "Four-level paging", "kmalloc/kfree work", "IDT loaded",
                 "PIC remapped", "Timer PIT", "Interrupts enabled",
                 "[INT3]", "The timer ticks", "A GDT with ring 3",
                 "System calls through the syscall instruction"]
        at = [log.find(s) for s in steps]
        for s, i in zip(steps, at):
            check(f"the message '{s}'", i >= 0)
        check("and they came in that order",
              all(i >= 0 for i in at) and at == sorted(at))

        m = re.search(r"The timer ticks: (\d+) -> (\d+) ticks", log)
        check("the PIT timer ticks on its own"
              + (f" ({m.group(1)} -> {m.group(2)})" if m else ""),
              m is not None and int(m.group(2)) > int(m.group(1)))

        # The hardware: each line says what was found rather than that
        # something was
        check("the empty disk was found and formatted with TFS",
              re.search(r"TFS: an empty disk formatted \(\d+ blocks", log)
              is not None)
        check("the disk controller is named at boot (here the old ATA PIO)",
              re.search(r"Disk ATA PIO \+ TFS ready \(\d+ sectors\)", log)
              is not None)
        # A machine with nothing on the auxiliary port says so instead
        # of reporting a mouse it has not got; the wheel is asked for
        # with a knock of sample rates, and QEMU's mouse answers it
        check("the PS/2 keyboard and mouse were found, and the wheel "
              "negotiated",
              "PS/2 keyboard (IRQ 1) and mouse (IRQ 12), wheel: yes" in log)
        check("the RTL8139 network card was found at boot",
              re.search(r"Network: RTL8139 ([0-9a-f]{1,2}:){5}", log)
              is not None)
        check("on one core the system says so outright",
              "Processors: 1" in log)

        # The kernel's console stays in text mode - the graphical
        # loading screen is for a boot to the desktop
        check("text-mode boot keeps the kernel console instead of the splash",
              "[boot] visual loading screen active" not in log)

        # The IP address comes from slirp's DHCP server after the shell
        # has started
        lease = q.wait_serial("[net] DHCP", timeout=30)
        time.sleep(0.5)
        lease = q.serial()
        check("DHCP granted an IP address", "granted 10.0.2.15" in lease)
        check("DHCP handed over a gateway and a DNS server",
              "gateway 10.0.2.2" in lease and "DNS 10.0.2.3" in lease)

        # The keyboard: inject a word and read the echo back. The keys
        # come from the string itself - a hand-written list of letters
        # is a trap for whoever renames the word. Shift+X has to come
        # out as a capital.
        seen = len(q.serial())
        typed = "thread 1"
        for ch in typed:
            q.sendkey("spc" if ch == " " else ch)
        q.sendkey("shift-x")
        time.sleep(0.5)
        echo = q.serial()[seen:]
        check(f"the keyboard echo: '{typed}'", typed in echo)
        check("the keyboard echo with shift: 'X'", typed + "X" in echo)
        q.sendkey("ret")
        end = time.time() + 20
        while time.time() < end and "Unknown" not in q.serial()[seen:]:
            time.sleep(0.3)
        check("the typed line reaches the shell, which reports it unknown",
              "Unknown command: %sX" % typed in q.serial()[seen:])
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
