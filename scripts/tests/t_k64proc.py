#!/usr/bin/env python3
"""ThreadOS smoke test: processes in ring 3 (64-bit kernel).

Only the processor knows the code selector, and its two lowest bits say
which ring the program REALLY ran in - and to say that, it had to enter
the kernel with a syscall instruction.

After that comes everything a process gets and everything it must not
do: a program read from a file (the kernel did not see it at build
time), the NX bit checked by executing its own data, a refusal for
pointers into kernel memory in every system call that takes one, two
processes at the same virtual address in separate spaces, and a fork
with copy on write - with a page bill that shows only what somebody
wrote gets copied, the kernel's own writes included.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import shutil
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64

# What wild64 hands the kernel a kernel pointer in, by the names it
# prints. The first buffers need nothing but the program, so it tries
# them at boot too. The rest come only with 'wild64 all <address>': the
# boot gives the program four seconds, and some of them need the disk,
# a child, a second thread or an address on the network.
WILD_BUFFERS = [
    "write(stdout, kernel memory)",
    "pipe(-> kernel memory)",
    "read(pipe -> kernel memory)",
    "time(-> kernel memory)",
    "sysinfo(-> kernel memory)",
    "getcwd(-> kernel memory)",
    "getcwd(-> NULL)",
    "netinfo(-> kernel memory)",
    "initrd_read(-> kernel memory)",
]
WILD_ALL = WILD_BUFFERS + [
    "ps(-> kernel memory)",
    "palette(-> kernel memory)",
    "ui_look(-> kernel memory)",
    "cfg_get(-> kernel memory)",
    "pci_info(-> kernel memory)",
    "clip_set(kernel memory)",
    "clip_get(-> kernel memory)",
    "trywait(code -> kernel memory)",
    "initrd_read(kernel name)",
    "mkdir(kernel name)",
    "chdir(kernel name)",
    "unlink(kernel name)",
    "open(kernel name)",
    "fdinfo(-> kernel memory)",
    "readdir(-> kernel memory)",
    "mmap_file(kernel name)",
    "mmap_file(size -> kernel memory)",
    "rename(kernel name)",
    "rename(-> kernel name)",
    "thread(kernel entry)",
    "udp_send(kernel memory)",
    "udp_recv(-> kernel memory)",
]


def wild_proven(text, name):
    return ("[wild64] %s: refused, and with an honest pointer it works"
            % name) in text


def kernel_address_of(path, text):
    """Where the running kernel holds 'text': read from the ELF file the
    loader is given, in the segments it loads. The kernel runs at the
    address it was linked for, so the file's address is the live one."""
    with open(path, "rb") as fh:
        img = fh.read()
    if img[:4] != b"\x7fELF" or img[4] != 2:
        return None
    phoff, = struct.unpack_from("<Q", img, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", img, 0x36)
    loaded = []
    for i in range(phnum):
        kind, _, offset, vaddr, _, filesz = struct.unpack_from(
            "<IIQQQQ", img, phoff + i * phentsize)
        if kind == 1:                       # PT_LOAD
            loaded.append((offset, vaddr, filesz))
    at = img.find(text)
    while at >= 0:
        for offset, vaddr, filesz in loaded:
            if offset <= at and at + len(text) <= offset + filesz:
                return vaddr + at - offset
        at = img.find(text, at + 1)
    return None


def wait_fresh_re(q, start, pattern, timeout):
    """What arrived on the serial port since 'start', once 'pattern'
    matches in it (or when the time is up) - a pattern that ends in the
    line's end waits for the whole line, not for its first half."""
    end = time.time() + timeout
    while time.time() < end:
        fresh = q.serial()[start:]
        if re.search(pattern, fresh):
            return fresh
        time.sleep(0.3)
    return q.serial()[start:]


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
            # The kernel names the pointer it turned away, so this is
            # the kernel image's address and not just any bad one
            check("the kernel refuses a pointer from outside the program",
                  "write: the pointer 100000 does not belong to the "
                  "program" in wild
                  and wild_proven(wild, "write(stdout, kernel memory)"))
            missing = [n for n in WILD_BUFFERS if not wild_proven(wild, n)]
            check("at boot every call with a buffer refuses a kernel "
                  "pointer and takes an honest one (%d of %d%s)"
                  % (len(WILD_BUFFERS) - len(missing), len(WILD_BUFFERS),
                     ", not: " + "; ".join(missing) if missing else ""),
                  not missing and "[wild64] the kernel refused all %d "
                  "kernel pointers - good" % len(WILD_BUFFERS) in wild)
            check("kernel memory is out of reach from ring 3",
                  "out of reach of ring 3" in wild)
            m = re.search(r"Kernel memory: .*err=([0-9a-f]+), "
                          r"address=([0-9a-f]+)", wild)
            check("the page fault says: a read from ring 3 of a kernel page"
                  + (f" (err={m.group(1)}, address={m.group(2)})"
                     if m else ""),
                  m is not None and int(m.group(1), 16) == 0x05
                  and int(m.group(2), 16) == 0x100000)
            # A refusal that killed the process would pass the checks
            # above too, and that is not the point: the program is to
            # survive the refused call and go on - and die only of its
            # own reaching, with nothing after it run
            check("the program survived the refused call and went on",
                  "[wild64] then I will reach in myself" in wild)
            check("the code after the attack never ran",
                  "ERROR: I read the kernel's memory" not in wild)

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
                  + (f" ({addrs[0][2]} and {addrs[1][2]})"
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
            child = m.group(1) if m else None
            check("the child gets zero out of the fork",
                  "child: the fork gave 0" in fk)
            # Both return to the same place with the same state,
            # but their memory is already separate
            check("the child reads what the parent wrote before the fork "
                  "(100)", "child: the fork gave 0, I inherited 100" in fk)
            check("the child writes over its own copy (200)",
                  "my value is 200" in fk)
            # The parent looks only once the child has ended, so a
            # write the child made late cannot slip past the look
            m = re.search(r"parent: waited for child (\d+), which ended "
                          r"with code 0", fk)
            check("the parent waits for that child, and it ends with no "
                  "error" + (f" ({m.group(1)})" if m else ""),
                  m is not None and m.group(1) == child)
            check("the child's write does not reach the parent (still 100)",
                  "parent: still 100 - the child got a copy" in fk)
            # A write the KERNEL makes for the child: read() fills a
            # page the child has not touched since the fork. The page
            # has to be split when the call checks its buffer, or on
            # the kernel's own fault. A kernel that did neither turns
            # the read away or stops on that fault - and where ring 0
            # writes through a read-only page, the bytes land in the
            # frame the parent sees.
            check("a kernel write in the child (read from a pipe) lands "
                  "in a private copy of a still-shared page",
                  "child: the kernel read 'written by the kernel' into my "
                  "copy of the shared page, and the rest of it is the "
                  "parent's" in fk)
            check("the kernel's write in the child does not reach the "
                  "parent's page",
                  "parent: my page is as I left it - the kernel wrote "
                  "into the child's copy" in fk)
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
                  + (f" (number {m.group(1)})" if m else ""),
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

            # Kernel pointers once more, now with the disk up: file
            # names and the rest that boot could not try. A refused
            # name proves nothing when no such file exists anyway, so
            # the name in kernel memory has to be a real one - the text
            # "wild64" in the kernel image names the program in the
            # initrd, and its first four bytes the file 'wild' that
            # wild64 makes for each attempt. The datagram it sends
            # itself for the honest twin of the UDP calls needs the
            # machine to have its address, so the lease comes first.
            q23.wait_serial("TFS ready", timeout=30)
            q23.wait_serial("[net] DHCP: granted", timeout=60)
            kname = kernel_address_of(KERNEL, b"wild64\0")
            check("the kernel image holds the text 'wild64'"
                  + (" (at %x)" % kname if kname else ""),
                  kname is not None)
            seen = len(q23.serial())
            q23.type_line("run wild64 all %x" % (kname or 0))
            wa = wait_fresh_re(q23, seen,
                               r"\[wild64\] done, exit code -?\d+ "
                               r"\(frames [^)]*\)\r?\n", timeout=90)
            for name in WILD_ALL:
                check("%s: a kernel pointer is refused, an honest one "
                      "works" % name, wild_proven(wa, name))
            check("the program found every refusal proven (%d)"
                  % len(WILD_ALL),
                  "[wild64] the kernel refused all %d kernel pointers - "
                  "good" % len(WILD_ALL) in wa)
            # The calls the kernel refused did not stop the program,
            # and its own reach for kernel memory still did
            check("after all of them the program still dies of its own "
                  "reach (code -1)",
                  "(wild64) killed: Page Fault" in wa
                  and "[wild64] done, exit code -1" in wa
                  and "ERROR: I read the kernel's memory" not in wa)
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
