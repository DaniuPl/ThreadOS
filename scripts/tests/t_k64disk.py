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
        print("SKIPPED: " + reason)
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
        check("the disc with the 64-bit kernel builds", make_iso64(d, iso))
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
            dsk = q23.wait_serial("] Disk ", timeout=30)
            m = re.search(r"\] Disk (.+?) \+ PFS (\w+) "
                          r"\((\d+) sectors\)", dsk)
            check("the 64-bit kernel finds the disk through PCI and AHCI"
                  + (f" ({m.group(1)}, {m.group(3)} sectors)"
                     if m else ""),
                  m is not None and "AHCI" in m.group(1)
                  and int(m.group(3)) > 0)
            check("the PFS comes up on the disk seen through AHCI",
                  m is not None and m.group(2) == "ready")

            q23.type_line("disk")
            io = q23.wait_serial("disk: sector", timeout=30)
            m = re.search(r"disk: sector (\d+) written and "
                          r"read back, differences (\d+)", io)
            check("writing and reading a sector through DMA give the same thing back"
                  + (f" (sector {m.group(1)})" if m else ""),
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
            hp = q23.wait_serial("[heap64] done, exit code",
                                 timeout=45)[seen:]
            m = re.search(r"asking for (\d+) KiB \((\d+) pages\)", hp)
            check("the program asks the kernel for memory as it runs"
                  + (f" ({m.group(1)} KiB)" if m else ""),
                  m is not None and int(m.group(2)) >= 1000)
            check("the memory gives back what the program wrote into it",
                  "of them, and every one gave back what I wrote" in hp)
            check("the heap break comes back once the memory is given back",
                  "the break came home" in hp)
            check("below its own heap the kernel does not let go",
                  "below its own heap the kernel does not let go" in hp)
            # And the bill: 1024 pages asked for, four used
            m2 = re.search(r"frames (\d+) -> (\d+), "
                           r"heap pages (\d+)", hp)
            check("the pages come into being only when they are touched"
                  + (f" (z {m.group(2) if m else '?'} "
                     f"asked for: {m2.group(3)})" if m2 else ""),
                  m2 is not None and 1 <= int(m2.group(3)) <= 8)
            check("not one frame is left after the program"
                  + (f" ({m2.group(1)} -> {m2.group(2)})"
                     if m2 else ""),
                  m2 is not None
                  and int(m2.group(2)) <= int(m2.group(1)))

            # The program's arguments. Until now 'echo' had to be
            # built into the shell, because there was no way of
            # passing a program a single word.
            seen = len(q23.serial())
            q23.type_line("run uecho64 one two three")
            ue = q23.wait_serial("[uecho64] done, exit code",
                                 timeout=40)[seen:]
            check("the program gets its arguments from the command line",
                  "one two three" in ue)
            check("and it ends with no error",
                  "[uecho64] done, exit code 0" in ue)

            # A tool that until recently had to sit inside the
            # kernel: it takes the file name from an argument, opens
            # it with a descriptor and reads
            q23.type_line("write zargv content for ucat")
            q23.wait_serial("write: 'zargv' written", timeout=30)
            seen = len(q23.serial())
            q23.type_line("run ucat64 zargv")
            uc = q23.wait_serial("[ucat64] done, exit code",
                                 timeout=40)[seen:]
            check("the program reads the file named in the argument",
                  "content for ucat" in uc
                  and "[ucat64] done, exit code 0" in uc)

            seen = len(q23.serial())
            q23.type_line("run ucat64")
            ub = q23.wait_serial("[ucat64] done, exit code",
                                 timeout=40)[seen:]
            check("with no argument the program says what it is missing",
                  "ucat64: give a file name" in ub
                  and "exit code 1" in ub)

            # The network: the card, the IP layer, DHCP and TCP
            # are again the same files as in the 32-bit kernel. Three
            # of the four came across without a single change - the
            # card driver only needed explicit casts of addresses,
            # because the card addresses memory with 32 bits.
            kar = q23.wait_serial("] Network: RTL8139", timeout=40)
            check("the 64-bit kernel finds the network card",
                  re.search(r"Network: RTL8139 ([0-9a-f]+:){5}",
                            kar) is not None)
            dh = q23.wait_serial("[net] DHCP", timeout=40)
            m = re.search(r"DHCP: granted (\d+\.\d+\.\d+\.\d+), "
                          r"gateway (\d+\.\d+\.\d+\.\d+)", dh)
            check("the IP address comes off the network through DHCP"
                  + (f" ({m.group(1)}, gateway {m.group(2)})"
                     if m else ""),
                  m is not None)

            seen = len(q23.serial())
            q23.type_line("ifconfig")
            ifc = q23.wait_serial("RX ", timeout=25)[seen:]
            check("ifconfig shows the address, netmask, gateway and DNS",
                  re.search(r"IP \d+\.\d+\.\d+\.\d+  netmask ", ifc)
                  is not None)

            # The gateway's answer is the only proof that frames really
            # go out and come back - the rest is only configuration
            seen = len(q23.serial())
            q23.type_line("ping 10.0.2.2")
            pg = q23.wait_serial("--- statistics", timeout=40)[seen:]
            m = re.search(r"sent (\d+), received (\d+)", pg)
            check("the gateway answers a ping"
                  + (f" ({m.group(2)}/{m.group(1)})" if m else ""),
                  m is not None and m.group(1) == m.group(2)
                  and int(m.group(2)) > 0)

            seen = len(q23.serial())
            q23.type_line("arp")
            ar = q23.wait_serial("10.0.2.2  ", timeout=25)[seen:]
            check("the ARP table knows the gateway's hardware address",
                  re.search(r"10\.0\.2\.2  ([0-9a-f]+:){5}", ar)
                  is not None)

            # Directories. The file system stopped being flat,
            # and the working directory belongs to the task - so a
            # program sees the one the shell stood in.
            q23.type_line("mkdir project")
            mk = q23.wait_serial("mkdir: 'project' created",
                                 timeout=25)
            check("the shell creates a directory",
                  "mkdir: 'project' created" in mk)
            seen = len(q23.serial())
            q23.type_line("cd project")
            q23.type_line("pwd")
            pw = q23.wait_serial("/project", timeout=25)[seen:]
            check("going into a directory changes the working path",
                  "/project" in pw)

            q23.type_line("write plan directories work")
            q23.wait_serial("write: 'plan' written", timeout=25)
            seen = len(q23.serial())
            q23.type_line("run uls64")
            ul = q23.wait_serial("uls64: entries", timeout=40)[seen:]
            check("program dziedziczy directory roboczy po shellu",
                  "uls64: /project" in ul)
            check("and it sees only its own entries in there",
                  "plan  (17 B)" in ul and "uls64: entries 1" in ul)

            seen = len(q23.serial())
            q23.type_line("cd ..")
            q23.type_line("run uls64")
            ul2 = q23.wait_serial("uls64: entries", timeout=40)[seen:]
            check("going up comes back to the root directory",
                  "uls64: /" in ul2)
            check("a directory shows as a directory, not a file",
                  "project/  (directory)" in ul2)

            q23.type_line("rm nosuchthing")
            rm = q23.wait_serial("rm: there is no", timeout=25)
            check("removing a file that is not there does not fake success",
                  "rm: there is no 'nosuchthing'" in rm)

            # A file as seen by the PROGRAM: a descriptor is an
            # ordinary number in its own table, and the inode that
            # number points at is known only to the kernel
            q23.type_line("run fileio64")
            fio = q23.wait_serial("[fileio64] done, exit code",
                                  timeout=45)
            check("a ring 3 program creates a file and writes into it",
                  "creating a file and writing into it from ring 3" in fio)
            m = re.search(r"match byte for byte \((\d+) B\)", fio)
            check("reading the file gives back what the program wrote"
                  + (f" ({m.group(1)} B)" if m else ""),
                  m is not None and int(m.group(1)) > 0)
            check("the kernel does not open a file that is not there",
                  "the kernel does not open a file that is not there" in fio)
            check("the program confirms the result with its exit code",
                  "[fileio64] done, exit code 0" in fio)

            # fs.c came across to long mode without a single
            # character changed - the disk seen through the block
            # layer looks the same in both modes
            q23.type_line("write note writes to the disk")
            wr = q23.wait_serial("write: 'note' written",
                                 timeout=30)
            check("the shell writes a file to the disk",
                  re.search(r"write: 'note' written \(\d+ B\)",
                            wr) is not None)
            q23.type_line("fls")
            fl = q23.wait_serial("Used:", timeout=25)
            check("the file shows in the root directory listing",
                  re.search(r"note  \(\d+ B\)", fl) is not None)
            seen = len(q23.serial())
            q23.type_line("cat note")
            ct = q23.wait_serial("writes to the disk", timeout=25)[seen:]
            check("reading the file gives back what was written",
                  "writes to the disk" in ct)

            ticks = q23.wait_serial("The timer ticks:", timeout=25)
            m = re.search(r"The timer ticks: (\d+) -> (\d+)", ticks)
            check("hardware interrupts arrive (the timer ticks"
                  + (f" {m.group(1)} -> {m.group(2)})" if m else ")"),
                  m is not None and int(m.group(2)) - int(m.group(1)) >= 100)
            # And the same on the screen: the console drawn by our own code
            w, h, at = q23.screen_raw()
            x0, y0 = (w - 640) // 2, (h - 200) // 2
            lit = sum(1 for y in range(y0, y0 + 200)
                      for x in range(x0, x0 + 640)
                      if at(x, y) != (0, 0, 0))
            check(f"the 64-bit console draws on the framebuffer "
                  f"({lit} pixels)", lit > 2000)
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
            check("a second boot finds the filesystem ready",
                  "+ PFS ready" in again
                  and "formatted" not in again)
            q2.wait_serial("proton64>", timeout=40)
            q2.type_line("fls")
            fl2 = q2.wait_serial("Used:", timeout=25)
            check("the file survived the machine being switched off",
                  re.search(r"note  \(\d+ B\)", fl2) is not None)
            seen = len(q2.serial())
            q2.type_line("cat note")
            ct2 = q2.wait_serial("writes to the disk",
                                   timeout=25)[seen:]
            check("and it holds exactly the same content",
                  "writes to the disk" in ct2)
            # A file written by a PROGRAM is to survive too - the
            # path through a descriptor ends on the same disk as the
            # path through the kernel
            seen = len(q2.serial())
            q2.type_line("cat zring3")
            ct3 = q2.wait_serial("written from ring 3",
                                   timeout=25)[seen:]
            check("a file written by a program survived the restart too",
                  "ProtonOS 64: a file written from ring 3" in ct3)
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
