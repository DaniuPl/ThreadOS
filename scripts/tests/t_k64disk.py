#!/usr/bin/env python3
"""ThreadOS smoke test: the disk, the file system and the network.

The disk sits behind an AHCI controller, whose registers lie high up,
outside the memory described at startup - without adding them to the
mapping the first read from a BAR would end in a page fault.

Then a second boot on THE SAME disk. That is the only proof that a file
really lies on the disk and not in memory - that anything survives the
machine being switched off.

At the end a third machine, started in text mode with its disk on the
old IDE channel: the boot line has to name that controller, and the
kernel's console has to stay on the screen instead of the loading
screen a desktop boot shows.

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
from ptest import _kernel_define as define

# The shell's report on a program's end, as kernel64/shell64.c prints it
REPORT = (r"\[%s\] done, exit code (-?\d+) \(frames (\d+) -> (\d+), "
          r"heap pages (\d+), stack (\d+), from files (\d+)\)\r?\n")
# How many pages heap64 touches on the heap and in the mapping area,
# how many it asks for in the latter, and how many more of each it has
# the kernel bring in by reading into them
TOUCH = define("user64/heap64.c", "TOUCH")
MAP_TOUCH = define("user64/heap64.c", "MAP_TOUCH")
MAP_ASK = define("user64/heap64.c", "MAP_ASK")
BY_KERNEL = define("user64/heap64.c", "BY_KERNEL")


def wait_serial_re(q, pattern, timeout=30, start=0):
    """The first match of 'pattern' on the serial port (from 'start'
    on), or None once the time is up - a line that arrives in pieces is
    waited for whole."""
    deadline = time.time() + timeout + q.boot_grace()
    while time.time() < deadline:
        m = re.search(pattern, q.serial()[start:])
        if m:
            return m
        time.sleep(0.3)
    return None


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
    # And an empty one for the IDE channel of the last machine
    ide_disk = tempfile.mktemp(suffix=".img")
    with open(ide_disk, "wb") as fh:
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
            q23.wait_serial("thread64>", timeout=90)

            # The disk. The controller's registers lie high up, outside
            # the memory described at startup - without adding them to
            # the mapping the first read from a BAR would end in a page
            # fault.
            # One line speaks about both layers: what the driver
            # found and whether the file system stood on it
            dsk = q23.wait_serial("] Disk ", timeout=30)
            m = re.search(r"\] Disk (.+?) \+ TFS (\w+) "
                          r"\((\d+) sectors\)", dsk)
            check("the 64-bit kernel finds the disk through PCI and AHCI"
                  + (f" ({m.group(1)}, {m.group(3)} sectors)"
                     if m else ""),
                  m is not None and "AHCI" in m.group(1)
                  and int(m.group(3)) > 0)
            check("the TFS comes up on the disk seen through AHCI",
                  m is not None and m.group(2) == "ready")

            # Writing a raw sector is the admin's business, so it goes
            # through 'sudo' and the password on the line it asks on
            q23.command("sudo disk", "password:", timeout=30)
            q23.type_line("thread")
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
            wait_serial_re(q23, REPORT % "heap64", timeout=60, start=seen)
            hp = q23.serial()[seen:]
            m = re.search(r"asking for (\d+) KiB \((\d+) pages\)", hp)
            check("the program asks the kernel for memory as it runs"
                  + (f" ({m.group(1)} KiB)" if m else ""),
                  m is not None and int(m.group(2)) >= 1000)
            # Before it writes anything. On a machine this young the
            # frames are most likely ones nobody has used yet, so this
            # is the plain case; t_mmap checks the same after heap64
            # has gone round the whole memory, where a page the kernel
            # did not clear would show somebody's old data.
            check("fresh heap pages read as zeros (%d of them)" % TOUCH,
                  "[heap64] the %d heap pages read as zeros before I "
                  "wrote to them" % TOUCH in hp)
            check("the memory gives back what the program wrote into it",
                  "of them, and every one gave back what I wrote" in hp)
            check("the heap break comes back once the memory is given back",
                  "the break came home" in hp)
            check("below its own heap the kernel does not let go",
                  "below its own heap the kernel does not let go" in hp)
            check("fresh pages of the mapping area read as zeros too",
                  "[heap64] the mapping area: asked for %d pages, touched "
                  "%d - zeros at first, then what I wrote"
                  % (MAP_ASK, MAP_TOUCH) in hp)
            # A page the program never touched is still its own, and
            # the first to touch it may be the kernel: a read() into
            # it. The call has to bring the page in rather than turn
            # the buffer away - and the page comes as clean as the
            # others.
            check("a read() into a heap page nobody has touched goes "
                  "through, and the page is zeros around what was read",
                  "[heap64] a page the kernel brought in on the heap: "
                  "zeros around what it read" in hp)
            check("the same in the mapping area",
                  "[heap64] a page the kernel brought in in the mapping "
                  "area: zeros around what it read" in hp)
            # And the bill: 1024 pages asked for, TOUCH used and
            # BY_KERNEL read into - nothing else in the program reaches
            # the heap, so not one page more. The shell's 'from files'
            # counts every page the mapping area brought in, the
            # anonymous ones included.
            m2 = re.search(REPORT % "heap64", hp)
            check("the pages come into being only when they are touched"
                  + (f" ({m2.group(4)} of {m.group(2) if m else '?'} "
                     f"asked for)" if m2 else ""),
                  m2 is not None and int(m2.group(4)) == TOUCH + BY_KERNEL)
            check("the mapping area pays for the pages touched too"
                  + (f" ({m2.group(6)} of {MAP_ASK})" if m2 else ""),
                  m2 is not None
                  and int(m2.group(6)) == MAP_TOUCH + BY_KERNEL)
            check("the program ends with no error",
                  m2 is not None and m2.group(1) == "0")
            check("not one frame is left after the program"
                  + (f" ({m2.group(2)} -> {m2.group(3)})"
                     if m2 else ""),
                  m2 is not None
                  and int(m2.group(3)) <= int(m2.group(2)))

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

            # The network: the card, the IP layer, DHCP and TCP. The
            # card driver needs explicit casts of addresses, because
            # the card addresses memory with 32 bits while a pointer
            # here is eight bytes wide.
            kar = q23.wait_serial("] Network: RTL8139", timeout=40)
            check("the 64-bit kernel finds the network card",
                  re.search(r"Network: RTL8139 ([0-9a-f]+:){5}",
                            kar) is not None)
            # The lease line is put together out of several printk
            # calls, so the wait is for all of it, up to its end -
            # otherwise the last address is read half-written
            m = wait_serial_re(q23, r"DHCP: granted (\d+\.\d+\.\d+\.\d+), "
                                    r"gateway (\d+\.\d+\.\d+\.\d+), "
                                    r"DNS (\d+\.\d+\.\d+\.\d+)\r?\n",
                               timeout=40)
            # slirp leases its first address, .15, to the first client
            # that asks - and this machine is the only one
            check("the IP address comes off the network through DHCP"
                  + (f" ({m.group(1)}, gateway {m.group(2)})"
                     if m else ""),
                  m is not None and m.group(1) == "10.0.2.15")
            # slirp hands out the gateway at .2 and its name server at
            # .3 - two different options of the same reply, and the
            # second one is what every name lookup later depends on
            check("DHCP handed over a gateway and a DNS server"
                  + (f" ({m.group(2)}, DNS {m.group(3)})" if m else ""),
                  m is not None and m.group(2) == "10.0.2.2"
                  and m.group(3) == "10.0.2.3")

            # The counters' line comes after the addresses' one, so once
            # it is whole the addresses are too
            seen = len(q23.serial())
            q23.type_line("ifconfig")
            wait_serial_re(q23, r"RX \d+ packets, TX \d+ packets, "
                                r"dropped \d+\+\d+\r?\n", timeout=25,
                           start=seen)
            ifc = q23.serial()[seen:]
            m = re.search(r"IP (\S+)  netmask (\S+)  gateway (\S+)  "
                          r"DNS (\S+)\r?\n", ifc)
            check("ifconfig shows the address, netmask, gateway and DNS"
                  + (" (%s)" % ", ".join(m.groups()) if m else ""),
                  m is not None and m.groups() == (
                      "10.0.2.15", "255.255.255.0", "10.0.2.2",
                      "10.0.2.3"))

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
            check("a program inherits the working directory from the shell",
                  "uls64: /project" in ul)
            check("and it sees only its own entries in there ('%s')"
                  % next((ln.strip() for ln in ul.splitlines()
                          if ln.strip().startswith("plan")), "no plan"),
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

            # The shell's own path to the file system, next to the
            # program's above: the same disk through the block layer
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
            again = q2.wait_serial("+ TFS", timeout=60)
            check("a second boot finds the filesystem ready",
                  "+ TFS ready" in again
                  and "formatted" not in again)
            q2.wait_serial("thread64>", timeout=40)
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
                  "ThreadOS 64: a file written from ring 3" in ct3)
        finally:
            q2.close()

        # A text-mode start with the disk on the IDE channel. The boot
        # line names the controller the block layer settled on, so a
        # disk there has to be reported as ATA PIO - not as the AHCI
        # the first machine had. And a machine told to stay in text
        # keeps the kernel's console: the loading screen is for a boot
        # into the desktop, and over a text console it would hide the
        # very lines such a boot is for.
        q3 = Qemu(KERNEL, INITRD, disk=ide_disk, append=TEXT_MODE)
        try:
            q3.wait_serial("thread64>", timeout=60)
            boot = q3.serial()
            m = re.search(r"\] Disk (.+?) \+ TFS (\w+) \((\d+) sectors\)",
                          boot)
            check("the disk controller is named at boot (here the old ATA PIO)"
                  + (f" ('{m.group(0)}')" if m else ""),
                  m is not None and m.group(1) == "ATA PIO"
                  and m.group(2) == "ready" and int(m.group(3)) > 0)
            # The splash would say so on the port before the kernel's
            # greeting, and the greeting proves the port caught the
            # start of the boot rather than missing it
            check("text-mode boot keeps the kernel console instead of the splash",
                  "The kernel is running in long mode" in boot
                  and "[boot] visual loading screen active" not in boot)
        finally:
            q3.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, slog2, disk, ide_disk):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
