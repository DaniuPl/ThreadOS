#!/usr/bin/env python3
"""ProtonOS smoke test: the 32-bit kernel's boot.

The first of the groups split out of the old collective 'rdzen'. It asks
about what happens BEFORE anybody types anything: did the kernel reach
the end of initialization, did it recognize the hardware, did the timer,
the scheduler and the network start.

We read the order of the messages from the serial port rather than from
the screen - from the screen the oldest lines scroll away, and a proof
that disappears when the machine slows down is no proof at all.

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
        screen = q.wait_for("ProtonOS shell")
        print("=== Ekran VGA po boocie ===")
        print(screen)
        print("===========================")

        check("dysk + PFS wykryty i sformatowany",
              "+ PFS gotowy" in screen)
        check("kontroler dysku nazwany przy boocie (tu stary ATA PIO)",
              "Dysk ATA PIO + PFS gotowy" in screen)
        check("mysz PS/2 wykryta przy boocie",
              "Mysz PS/2 gotowa" in screen)
        check("karta sieciowa RTL8139 wykryta przy boocie",
              "Siec: RTL8139" in screen)
        check("na jednym rdzeniu system mowi to wprost",
              "Procesory: 1" in q.serial())

        # The IP address comes from slirp's DHCP server only after the
        # shell has started. We wait for the message before the screen
        # starts serving the keyboard tests - otherwise it falls into the
        # middle of a line being typed.
        q.wait_for("[siec] DHCP", timeout=20)
        # The message is assembled out of several printk calls, so a
        # moment for the rest of the line to be added - otherwise the test
        # reads it half-finished
        time.sleep(0.5)
        lease = q.screen()
        check("DHCP przyznal adres IP", "przyznano 10.0.2.15" in lease)
        check("DHCP przekazal brame i serwer DNS",
              "brama 10.0.2.2" in lease and "DNS 10.0.2.3" in lease)

        log_text = q.serial()
        for expected in ["Multiboot OK", "GDT zaladowana", "IDT zaladowana",
                         "PIC przemapowany", "Timer PIT", "Klawiatura PS/2",
                         "Przerwania wlaczone"]:
            check(f"komunikat '{expected}'", expected in log_text)

        check("obsluga wyjatku int3 (breakpoint)", "[INT3]" in log_text)

        for expected in ["Pamiec fizyczna", "Stronicowanie wlaczone",
                         "kmalloc/kfree dzialaja"]:
            check(f"komunikat '{expected}'", expected in screen)

        check("jadro przezylo init pamieci (doszlo do shella)",
              "ProtonOS shell" in screen)

        # Thread C ends after two seconds. Its text in line zero
        # disappears with every scroll of the screen, so we ask the serial
        # port about it: nothing scrolls away there.
        end_c = q.wait_serial("Watek demonstracyjny C zakonczyl prace",
                              timeout=30)
        check("watek C zakonczyl prace",
              "Watek demonstracyjny C zakonczyl prace" in end_c)

        # The timer: the uptime in the screen's corner has to grow
        def uptime(s):
            m = re.search(r"uptime: (\d+)s", s)
            return int(m.group(1)) if m else None

        t1 = uptime(q.wait_for("uptime: "))
        time.sleep(2)
        t2 = uptime(q.screen())
        check(f"timer PIT tyka (uptime {t1}s -> {t2}s)",
              t1 is not None and t2 is not None and t2 > t1)

        # Multitasking: the counters of threads A and B have to grow
        # independently. We take the LAST occurrence - after the screen
        # scrolls, old, dead copies of the line may hang higher up.
        def counters(s):
            a = re.findall(r"watek-A: (\d+)", s)
            b = re.findall(r"watek-B: (\d+)", s)
            return (int(a[-1]) if a else None, int(b[-1]) if b else None)

        a1, b1 = counters(q.wait_for("watek-A: "))
        time.sleep(1.5)
        a2, b2 = counters(q.screen())
        check(f"watek A liczy ({a1} -> {a2})",
              a1 is not None and a2 is not None and a2 > a1)
        check(f"watek B liczy ({b1} -> {b2})",
              b1 is not None and b2 is not None and b2 > b1)

        # The keyboard: inject "proton 1" and check the echo
        for key in ["p", "r", "o", "t", "o", "n", "spc", "1"]:
            q.sendkey(key)
        q.sendkey("shift-x")  # a shift test: 'X' should come out
        screen = q.screen()
        check("echo klawiatury: 'proton 1'", "> proton 1" in screen)
        check("echo klawiatury z shiftem: 'X'", "> proton 1X" in screen)

        q.sendkey("ret")
        time.sleep(0.3)
        check("nieznana komenda zgloszona",
              "nieznana komenda: 'proton'" in q.screen())
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
