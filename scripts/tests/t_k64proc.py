#!/usr/bin/env python3
"""ThreadOS smoke test: processes in ring 3 (64-bit kernel).

Only the processor knows the code selector, and its two lowest bits say
which ring the program REALLY ran in - and to say that, it had to enter
the kernel with a syscall instruction.

After that comes everything a process gets and everything it must not
do: a program read from a file (the kernel did not see it at build
time), the NX bit checked by executing its own data, a refusal for
pointers into kernel memory, two processes at the same virtual address
in separate spaces, and a fork with copy on write - with a page bill
that shows only what somebody wrote gets copied.

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
            q23.wait_serial("thread64>", timeout=90)

            # Ring 3: only the processor knows the code selector,
            # and its two lowest bits say which ring the program
            # REALLY ran in. To say that, it had to enter the kernel
            # with a syscall instruction.
            q23.wait_serial("System calls through the syscall instruction",
                            timeout=20)
            r3 = q23.wait_serial("A program in ring 3:", timeout=25)
            m = re.search(r"A program in ring 3: (.+?), cs=(\w+), "
                          r"ring (\d)", r3)
            check("the program entered the kernel with the syscall instruction",
                  m is not None and m.group(1) == "answered")
            check("the processor confirms ring 3"
                  + (f" (cs={m.group(2)})" if m else ""),
                  m is not None and m.group(3) == "3")

            # A program from a FILE: the kernel did not see it at
            # build time - it learns everything from the ELF header
            ini = q23.wait_serial("initrd:", timeout=25)
            check("the 64-bit kernel reads the initrd from the loader",
                  re.search(r"initrd: \d+ file", ini) is not None)
            ld = q23.wait_serial("from initrd:", timeout=25)
            m = re.search(r"Program 'hello64' from initrd: (\d+) B, "
                          r"entry ([0-9a-f]+)", ld)
            check("the ELF64 from the initrd was loaded"
                  + (f" ({m.group(1)} B, entry {m.group(2)})"
                     if m else ""),
                  m is not None and int(m.group(1)) > 0
                  # the entry point has to lie in user space, that
                  # is, from 1 GiB upwards
                  and int(m.group(2), 16) >= 0x40000000)
            out64 = q23.wait_serial("[hello64] ending", timeout=30)
            check("a program from a file writes through a system call",
                  "[hello64] hello, I am a 64-bit ELF"
                  in out64)
            check("the program counts and puts itself to sleep",
                  "[hello64] counting: 1" in out64
                  and "[hello64] counting: 3" in out64)
            m = re.search(r"ticks gone by: (\d+)", out64)
            check("the program's sleep lasts as long as it should"
                  + (f" ({m.group(1)} ticks)" if m else ""),
                  m is not None and 13 <= int(m.group(1)) <= 25)
            fin = q23.wait_serial("ELF64 from the initrd:", timeout=25)
            m = re.search(r"ring (\d), exit code (\d+)", fin)
            check("the program from the file ran in ring 3",
                  m is not None and m.group(1) == "3")
            check("the program's exit code reached the kernel"
                  + (f" ({m.group(2)})" if m else ""),
                  m is not None and m.group(2) == "7")

            # Memory protection as seen from the program. The NX
            # bit had only been set so far - here a program really
            # tries to execute its data and dies.
            nx = q23.wait_serial("Execute-disable:", timeout=30)
            check("the program tries to execute its own data",
                  "[nx64] and trying to execute it" in nx)
            check("the processor refuses to execute data (the NX bit)",
                  "the processor refused to execute data" in nx)
            # 0x15 = page present, access from ring 3, an
            # instruction fetch. The present bit alone would rule out
            # an ordinary missing mapping - this really was NX.
            m = re.search(r"Execute-disable: .*err=([0-9a-f]+)", nx)
            check("the page fault says: an instruction fetch from ring 3"
                  + (f" (err={m.group(1)})" if m else ""),
                  m is not None and int(m.group(1), 16) & 0x15 == 0x15)
            check("the guilty program is killed, not the whole system",
                  "killed: Page Fault" in nx)

            # The other road to kernel memory: ask the kernel
            # itself for it. It has to refuse, because it checks the
            # pointer.
            wild = q23.wait_serial("Kernel memory:", timeout=30)
            check("the kernel refuses a pointer from outside the program",
                  "does not belong to the program" in wild
                  and "[wild64] the kernel refused" in wild)
            check("kernel memory is out of reach from ring 3",
                  "out of reach of ring 3" in wild)
            m = re.search(r"Kernel memory: .*err=([0-9a-f]+), "
                          r"address=([0-9a-f]+)", wild)
            check("the page fault says: a read from ring 3 of a kernel page"
                  + (f" (err={m.group(1)}, address={m.group(2)})"
                     if m else ""),
                  m is not None and int(m.group(1), 16) == 0x05
                  and int(m.group(2), 16) == 0x100000)

            # And what this whole handling exists for
            surv = q23.wait_serial("The system survived", timeout=30)
            m = re.search(r"counter (\d+) -> (\d+)", surv)
            check("the system runs on after both programs die"
                  + (f" (counter {m.group(1)} -> {m.group(2)})"
                     if m else ""),
                  m is not None
                  and int(m.group(2)) > int(m.group(1)))

            # Two processes at once, each in its own space. The
            # same file, the same virtual address, separate memory -
            # and that is exactly what has to be visible.
            sep = q23.wait_serial("The processes' memory", timeout=40)
            addrs = re.findall(r"process (\d): address ([0-9a-f]+), "
                               r"I wrote (\d+)", sep)
            check(f"two processes started at once ({len(addrs)})",
                  len(addrs) == 2)
            check("both write to THE SAME virtual address"
                  + (f" ({addrs[0][1]})" if addrs else ""),
                  len(addrs) == 2 and addrs[0][1] == addrs[1][1])
            check("each wrote something different"
                  + (f" ({addrs[0][2]} i {addrs[1][2]})"
                     if len(addrs) == 2 else ""),
                  len(addrs) == 2 and addrs[0][2] != addrs[1][2])
            kept = re.findall(r"process \d: still (\d+) - my "
                              r"memory is untouched", sep)
            check(f"neither saw the other's memory "
                  f"({len(kept)}/2)", len(kept) == 2)
            check("both copies finished with no error",
                  "Two processes at once: 2 finished, 0 errors"
                  in sep)
            # The space has to disappear along with the process,
            # otherwise every program that ran takes memory forever
            m = re.search(r"The processes' memory came back "
                          r"\((\d+) -> (\d+) frames\)", sep)
            check("the processes' memory came back to the pool"
                  + (f" ({m.group(1)} -> {m.group(2)})"
                     if m else ""),
                  m is not None
                  and int(m.group(2)) <= int(m.group(1)))

            # fork: the second process comes no longer from the
            # kernel's decision but from the program's own
            fk = q23.wait_serial("The fork's memory", timeout=45)
            m = re.search(r"parent: the fork gave the child number (\d+)", fk)
            check("the parent gets the child's number out of the fork"
                  + (f" ({m.group(1)})" if m else ""),
                  m is not None and int(m.group(1)) > 0)
            check("the child gets zero out of the fork",
                  "child: the fork gave 0" in fk)
            # Both return to the same place with the same state,
            # but their memory is already separate
            check("the child writes over its own copy (200)",
                  "my value is 200" in fk)
            check("the child's write does not reach the parent (still 100)",
                  "parent: still 100 - the child got a copy" in fk)
            check("both branches finished with no error",
                  "A process copied itself: 2 finished, "
                  "0 errors" in fk)
            m = re.search(r"The fork's memory came back "
                          r"\((\d+) -> (\d+) frames\)", fk)
            check("the copied memory came back to the pool"
                  + (f" ({m.group(1)} -> {m.group(2)})"
                     if m else ""),
                  m is not None
                  and int(m.group(2)) <= int(m.group(1)))

            # fork + exec + wait: the classic three that are
            # enough for every shell in the world
            sp = q23.wait_serial("fork + exec + wait:", timeout=60)
            check("a process starts ANOTHER program through exec",
                  "copying myself and running 'hello64' in the child"
                  in sp
                  and "[hello64] hello, I am a" in sp)
            m = re.search(r"I waited out the child (\d+), "
                          r"exit code (\d+)", sp)
            check("the parent waited out its own child"
                  + (f" (nr {m.group(1)})" if m else ""),
                  m is not None and int(m.group(1)) > 0)
            # hello64 ends with a seven - that seven is the proof
            # that it waited for THAT child, not for any finished task
            check("the exit code reached the parent through wait"
                  + (f" ({m.group(2)})" if m else ""),
                  m is not None and m.group(2) == "7")
            check("wait with no children says there is nothing to wait for",
                  "ERROR: wait invented a child" not in sp)
            check("the program confirms all three itself",
                  "[spawn64] fork + exec + wait work" in sp)
            check("the kernel confirms the program's verdict",
                  "fork + exec + wait: 2 finished, verdict 0"
                  in sp)

            # And now the bill: fork does NOT copy memory. The
            # program has seven pages (code, constants, data, four
            # stack pages), so copying outright would cost seven of
            # them plus the tables. With shared pages only the tables
            # are left, and only what somebody really wrote gets
            # copied.
            cow = q23.wait_serial("Copy on write",
                                  timeout=30)
            m = re.search(r"the fork took (\d+) frames, "
                          r"(\d+) pages were split", cow)
            check("the fork does not copy the program's memory"
                  + (f" ({m.group(1)} frames - the tables alone)"
                     if m else ""),
                  m is not None and int(m.group(1)) <= 6)
            check("only the pages really written were split"
                  + (f" ({m.group(2)})" if m else ""),
                  m is not None and 1 <= int(m.group(2)) <= 6)
        finally:
            q23.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, slog2, disk):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
