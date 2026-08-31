#!/usr/bin/env python3
"""ProtonOS smoke test: the disk, the file system and the network
(64-bit kernel).

The same drivers as in the 32-bit kernel, compiled a second time. The
controller's registers lie high up, outside the memory described at
startup - without adding them to the mapping the first read from a BAR
would end in a page fault.

At the end a second boot on THE SAME disk. That is the only proof that a
file really lies on the disk and not in memory - and the first moment at
which anything in this kernel survives the machine being switched off.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
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
    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")
    # A disk behind an AHCI controller: it was this path that required
    # adding the controller's registers to the mapping
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.write(b"\0" * DISK_SIZE)

    try:
        check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        q23 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                   sata=True, qemu="qemu-system-x86_64")
        try:
            # The kernel reaches the shell by itself; everything this
            # group checks happens only afterwards
            q23.wait_serial("proton64>", timeout=90)

            # The disk: the same drivers as in the 32-bit kernel,
            # compiled a second time. The controller's registers lie
            # high up, outside the memory described at startup -
            # without adding them to the mapping the first read from
            # a BAR would end in a page fault.
            # One line speaks about both layers: what the driver
            # found and whether the file system stood on it
            dsk = q23.wait_serial("] Dysk ", timeout=30)
            m = re.search(r"\] Dysk (.+?) \+ PFS (\w+) "
                          r"\((\d+) sektorow\)", dsk)
            check("jadro 64-bitowe znajduje dysk przez PCI i AHCI"
                  + (f" ({m.group(1)}, {m.group(3)} sektorow)"
                     if m else ""),
                  m is not None and "AHCI" in m.group(1)
                  and int(m.group(3)) > 0)
            check("PFS staje na dysku widzianym przez AHCI",
                  m is not None and m.group(2) == "gotowy")

            q23.type_line("disk")
            io = q23.wait_serial("disk: sektor", timeout=30)
            m = re.search(r"disk: sektor (\d+) zapisany i "
                          r"odczytany, roznic (\d+)", io)
            check("zapis i odczyt sektora przez DMA oddaja to samo"
                  + (f" (sektor {m.group(1)})" if m else ""),
                  m is not None and m.group(2) == "0")

            # The program's heap. Asking for memory costs
            # nothing - payment is for the pages actually touched,
            # because they come into being only under your fingers.
            #
            # This program is also a test of SSE in user code along
            # the way: the compiler vectorizes its loop copying 4 KiB
            # and puts a movdqa there, which without SSE enabled ends
            # in an exception.
            seen = len(q23.serial())
            q23.type_line("run heap64")
            hp = q23.wait_serial("[heap64] koniec, kod wyjscia",
                                 timeout=45)[seen:]
            m = re.search(r"prosze o (\d+) KiB \((\d+) stron\)", hp)
            check("program prosi jadro o pamiec w biegu"
                  + (f" ({m.group(1)} KiB)" if m else ""),
                  m is not None and int(m.group(2)) >= 1000)
            check("pamiec oddaje to, co program w nia wpisal",
                  "wszystkie oddaly to, co wpisalem" in hp)
            check("granica sterty wraca po oddaniu pamieci",
                  "granica wrocila na miejsce" in hp)
            check("ponizej wlasnej sterty jadro nie puszcza",
                  "ponizej wlasnej sterty jadro nie puszcza" in hp)
            # And the bill: 1024 pages asked for, four used
            m2 = re.search(r"ramek (\d+) -> (\d+), "
                           r"stron sterty (\d+)", hp)
            check("strony powstaja dopiero przy dotknieciu"
                  + (f" (z {m.group(2) if m else '?'} "
                     f"poproszonych: {m2.group(3)})" if m2 else ""),
                  m2 is not None and 1 <= int(m2.group(3)) <= 8)
            check("po programie nie zostaje ani jedna ramka"
                  + (f" ({m2.group(1)} -> {m2.group(2)})"
                     if m2 else ""),
                  m2 is not None
                  and int(m2.group(2)) <= int(m2.group(1)))

            # The program's arguments. Until now 'echo' had to be
            # built into the shell, because there was no way of
            # passing a program a single word.
            seen = len(q23.serial())
            q23.type_line("run uecho64 raz dwa trzy")
            ue = q23.wait_serial("[uecho64] koniec, kod wyjscia",
                                 timeout=40)[seen:]
            check("program dostaje argumenty z linii polecen",
                  "raz dwa trzy" in ue)
            check("i konczy sie bez bledu",
                  "[uecho64] koniec, kod wyjscia 0" in ue)

            # A tool that until recently had to sit inside the
            # kernel: it takes the file name from an argument, opens
            # it with a descriptor and reads
            q23.type_line("write zargv tresc dla ucat")
            q23.wait_serial("write: 'zargv' zapisany", timeout=30)
            seen = len(q23.serial())
            q23.type_line("run ucat64 zargv")
            uc = q23.wait_serial("[ucat64] koniec, kod wyjscia",
                                 timeout=40)[seen:]
            check("program czyta plik podany w argumencie",
                  "tresc dla ucat" in uc
                  and "[ucat64] koniec, kod wyjscia 0" in uc)

            seen = len(q23.serial())
            q23.type_line("run ucat64")
            ub = q23.wait_serial("[ucat64] koniec, kod wyjscia",
                                 timeout=40)[seen:]
            check("bez argumentu program mowi, czego mu brakuje",
                  "ucat64: podaj nazwe pliku" in ub
                  and "kod wyjscia 1" in ub)

            # The network: the card, the IP layer, DHCP and TCP
            # are again the same files as in the 32-bit kernel. Three
            # of the four came across without a single change - the
            # card driver only needed explicit casts of addresses,
            # because the card addresses memory with 32 bits.
            kar = q23.wait_serial("] Siec: RTL8139", timeout=40)
            check("jadro 64-bitowe znajduje karte sieciowa",
                  re.search(r"Siec: RTL8139 ([0-9a-f]+:){5}",
                            kar) is not None)
            dh = q23.wait_serial("[siec] DHCP", timeout=40)
            m = re.search(r"DHCP: przyznano (\d+\.\d+\.\d+\.\d+), "
                          r"brama (\d+\.\d+\.\d+\.\d+)", dh)
            check("adres IP przychodzi z sieci przez DHCP"
                  + (f" ({m.group(1)}, brama {m.group(2)})"
                     if m else ""),
                  m is not None)

            seen = len(q23.serial())
            q23.type_line("ifconfig")
            ifc = q23.wait_serial("RX ", timeout=25)[seen:]
            check("ifconfig pokazuje adres, maske, brame i DNS",
                  re.search(r"IP \d+\.\d+\.\d+\.\d+  maska ", ifc)
                  is not None)

            # The gateway's answer is the only proof that frames really
            # go out and come back - the rest is only configuration
            seen = len(q23.serial())
            q23.type_line("ping 10.0.2.2")
            pg = q23.wait_serial("--- statystyki", timeout=40)[seen:]
            m = re.search(r"wyslano (\d+), odebrano (\d+)", pg)
            check("brama odpowiada na ping"
                  + (f" ({m.group(2)}/{m.group(1)})" if m else ""),
                  m is not None and m.group(1) == m.group(2)
                  and int(m.group(2)) > 0)

            seen = len(q23.serial())
            q23.type_line("arp")
            ar = q23.wait_serial("10.0.2.2  ", timeout=25)[seen:]
            check("tablica ARP zna adres sprzetowy bramy",
                  re.search(r"10\.0\.2\.2  ([0-9a-f]+:){5}", ar)
                  is not None)

            # Directories. The file system stopped being flat,
            # and the working directory belongs to the task - so a
            # program sees the one the shell stood in.
            q23.type_line("mkdir projekt")
            mk = q23.wait_serial("mkdir: 'projekt' zalozony",
                                 timeout=25)
            check("shell zaklada katalog",
                  "mkdir: 'projekt' zalozony" in mk)
            seen = len(q23.serial())
            q23.type_line("cd projekt")
            q23.type_line("pwd")
            pw = q23.wait_serial("/projekt", timeout=25)[seen:]
            check("wejscie do katalogu zmienia sciezke robocza",
                  "/projekt" in pw)

            q23.type_line("write plan katalogi dzialaja")
            q23.wait_serial("write: 'plan' zapisany", timeout=25)
            seen = len(q23.serial())
            q23.type_line("run uls64")
            ul = q23.wait_serial("uls64: wpisow", timeout=40)[seen:]
            check("program dziedziczy katalog roboczy po shellu",
                  "uls64: /projekt" in ul)
            check("i widzi w nim tylko wlasne wpisy",
                  "plan  (18 B)" in ul and "uls64: wpisow 1" in ul)

            seen = len(q23.serial())
            q23.type_line("cd ..")
            q23.type_line("run uls64")
            ul2 = q23.wait_serial("uls64: wpisow", timeout=40)[seen:]
            check("wyjscie w gore wraca do katalogu glownego",
                  "uls64: /" in ul2)
            check("katalog jest widoczny jako katalog, nie plik",
                  "projekt/  (katalog)" in ul2)

            q23.type_line("rm nieistnieje")
            rm = q23.wait_serial("rm: nie ma", timeout=25)
            check("usuwanie nieistniejacego pliku nie udaje sukcesu",
                  "rm: nie ma 'nieistnieje'" in rm)

            # A file as seen by the PROGRAM: a descriptor is an
            # ordinary number in its own table, and the inode that
            # number points at is known only to the kernel
            q23.type_line("run fileio64")
            fio = q23.wait_serial("[fileio64] koniec, kod wyjscia",
                                  timeout=45)
            check("program w ring 3 zaklada plik i pisze do niego",
                  "zakladam plik i pisze do niego z ring 3" in fio)
            m = re.search(r"zgadza sie co do bajtu \((\d+) B\)", fio)
            check("odczyt z pliku oddaje to, co program zapisal"
                  + (f" ({m.group(1)} B)" if m else ""),
                  m is not None and int(m.group(1)) > 0)
            check("jadro nie otwiera pliku, ktorego nie ma",
                  "nieistniejacego pliku jadro nie otwiera" in fio)
            check("program sam potwierdza wynik kodem wyjscia",
                  "[fileio64] koniec, kod wyjscia 0" in fio)

            # fs.c came across to long mode without a single
            # character changed - the disk seen through the block
            # layer looks the same in both modes
            q23.type_line("write notatka pisze na dysk")
            wr = q23.wait_serial("write: 'notatka' zapisany",
                                 timeout=30)
            check("shell zapisuje plik na dysku",
                  re.search(r"write: 'notatka' zapisany \(\d+ B\)",
                            wr) is not None)
            q23.type_line("fls")
            fl = q23.wait_serial("Zajetosc:", timeout=25)
            check("plik widac na liscie katalogu glownego",
                  re.search(r"notatka  \(\d+ B\)", fl) is not None)
            seen = len(q23.serial())
            q23.type_line("cat notatka")
            ct = q23.wait_serial("pisze na dysk", timeout=25)[seen:]
            check("odczyt pliku oddaje to, co zapisano",
                  "pisze na dysk" in ct)

            ticks = q23.wait_serial("Timer tyka:", timeout=25)
            m = re.search(r"Timer tyka: (\d+) -> (\d+)", ticks)
            check("przerwania sprzetowe dochodza (timer tyka"
                  + (f" {m.group(1)} -> {m.group(2)})" if m else ")"),
                  m is not None and int(m.group(2)) - int(m.group(1)) >= 100)
            # And the same on the screen: the console drawn by our own code
            w, h, at = q23.screen_raw()
            x0, y0 = (w - 640) // 2, (h - 200) // 2
            lit = sum(1 for y in range(y0, y0 + 200)
                      for x in range(x0, x0 + 640)
                      if at(x, y) != (0, 0, 0))
            check(f"konsola 64-bitowa rysuje na buforze obrazu "
                  f"({lit} pikseli)", lit > 2000)
        finally:
            q23.close()

        # A second boot on THE SAME disk. That is the only proof that a
        # file really lies on the disk and not in memory - and the first
        # moment at which anything in this kernel survives the machine
        # being switched off.
        q2 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog2, disk=disk,
                  sata=True, qemu="qemu-system-x86_64")
        try:
            again = q2.wait_serial("+ PFS", timeout=60)
            check("drugi rozruch zastaje gotowy system plikow",
                  "+ PFS gotowy" in again
                  and "sformatowany" not in again)
            q2.wait_serial("proton64>", timeout=40)
            q2.type_line("fls")
            fl2 = q2.wait_serial("Zajetosc:", timeout=25)
            check("plik przezyl wylaczenie maszyny",
                  re.search(r"notatka  \(\d+ B\)", fl2) is not None)
            seen = len(q2.serial())
            q2.type_line("cat notatka")
            ct2 = q2.wait_serial("pisze na dysk",
                                   timeout=25)[seen:]
            check("i ma dokladnie te sama tresc",
                  "pisze na dysk" in ct2)
            # A file written by a PROGRAM is to survive too - the
            # path through a descriptor ends on the same disk as the
            # path through the kernel
            seen = len(q2.serial())
            q2.type_line("cat zring3")
            ct3 = q2.wait_serial("plik zapisany z ring 3",
                                   timeout=25)[seen:]
            check("plik zapisany przez program tez przezyl restart",
                  "ProtonOS 64: plik zapisany z ring 3" in ct3)
        finally:
            q2.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, slog2, disk):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
