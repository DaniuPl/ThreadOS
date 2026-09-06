#!/usr/bin/env python3
"""ProtonOS smoke test: Bluetooth - what can be checked without one.

This group is different from every other one in this project, and it
says so in its own output.

QEMU emulates no Bluetooth adapter: the whole stack was thrown out of
it years ago, and no other hypervisor has one either, because a guest
gets its network through a card that looks wired. So the conversation
with an adapter cannot be tested here at all - not badly, not
partially, not at all.

What CAN be tested is the half that is pure bytes: building commands
and reading events. Recorded packets of the shape the specification
gives go through the same parser the transport uses, and what comes out
has to be the address, the name, the version and the devices that were
in those bytes. A parser that reads an address backwards, or takes the
manufacturer from one byte too early, fails here - and those are
exactly the mistakes that would otherwise be found on a laptop, one
reboot at a time.

The second thing this checks is honesty: on a machine with no adapter,
the system has to SAY there is no adapter, and say why, rather than
show an empty list of devices as though it had looked.

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, make_iso64


def main():
    print()
    print("--- Bluetooth (pakiety, bo adaptera nie ma czym udawac) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)

    try:
        check("plyta z jadrem 64-bitowym buduje sie",
              make_iso64(d, iso, text=True))
        if not os.path.exists(iso):
            return summary()

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, usbkbd="xhci", qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=90)

            # --- the machine has no adapter, and has to say so --------
            out = q.command("bt", "bt:", timeout=20)
            check("system nie udaje, ze ma Bluetooth",
                  "nie ma adaptera" in out)
            check("i mowi, dlaczego go nie ma",
                  "adaptera Bluetooth na magistrali USB" in out
                  or "nie ma kontrolera USB" in out)

            said = q.command("sprzet", "PCI:", timeout=30)
            check("'sprzet' tez wymienia Bluetooth wsrod tego, czego brak",
                  "Bluetooth: brak" in said)

            # --- the packets ------------------------------------------
            out = q.command("bt test", "uslyszane", timeout=30)
            check("adres adaptera odczytany z pakietu we wlasciwa strone",
                  "adres adaptera: 00:1a:7d:da:71:13" in out)
            check("nazwa adaptera wyciagnieta z odpowiedzi",
                  "nazwa adaptera: Proton" in out)
            check("wersja i wytworca z Read Local Version",
                  "wersja: Bluetooth 5.0" in out and "000f" in out)
            check("dwa urzadzenia z dwoch roznych rodzajow zdarzen",
                  "uslyszane urzadzenia: 2" in out)
            check("urzadzenie LE z nazwa z rozglaszania i sila sygnalu",
                  "cb:a9:87:65:43:21" in out and "Sluchawki" in out
                  and "-59 dBm" in out)
            check("urzadzenie klasyczne z wyniku inquiry",
                  "66:55:44:33:22:11" in out)
            check("i test mowi wprost, czego NIE sprawdza",
                  "nie rozmowe z adapterem" in out)

            # A scan with no adapter must refuse, not pretend
            out = q.command("bt skan 1", "bt:", timeout=30)
            check("skan bez adaptera odmawia zamiast pokazywac pusta liste",
                  "nie ma adaptera" in out
                  and "uslyszanych urzadzen" not in out)
        finally:
            q.close()

        # The same code compiled for the other mode - 'sprzet' says the
        # same thing there, because it is the same file.
        disk32 = tempfile.mktemp(suffix=".img")
        with open(disk32, "wb") as fh:
            fh.truncate(DISK_SIZE)
        q = Qemu(KERNEL, INITRD, disk32, append=TEXT_MODE,
                 serial=tempfile.mktemp(suffix=".log"))
        try:
            q.wait_for("ProtonOS shell")
            q.type_line("sprzet")
            screen = q.wait_for("Bluetooth:", timeout=30)
            check("w trybie 32-bitowym ten sam wiersz o Bluetooth",
                  "Bluetooth: brak" in screen)
        finally:
            q.close()
            os.unlink(disk32)
    finally:
        for path in (iso, slog, disk):
            if os.path.exists(path):
                os.unlink(path)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
