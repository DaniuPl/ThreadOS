#!/usr/bin/env python3
"""ThreadOS smoke test: the TFS file system on the disk.

A file written by a ring 3 process and a file written from the kernel's
shell are to be the same file seen by both - and both are to survive a
restart of the machine. That last part matters most here: everything
else can be faked with a cache, and a SECOND BOOT on the same disk image
cannot. The disk sits on the IDE controller here; t_k64disk does the
same through AHCI.

At the end of the first boot we call 'crash', that is, a deliberate page
fault in the kernel. The diagnostic is to give the exception number and
the address from CR2, and the system is to stop rather than blunder on -
the whole machine, not just the shell that ran it: a second console,
which answered a moment before, falls silent too.

On the second boot a file gets a new name through the 'core' program's
'mv' (user/core.c), which asks the kernel for a rename - and the kernel
refuses a name that is taken and a source that is not there.

The machine stays at its text shell; what it says is read off the serial
port, and every answer is taken from what arrived after the command, so
the echo of an earlier line cannot pass for a result. Only the consoles
are read off the screen: which one is shown, and which one answered.

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

PROMPT = "thread64>"
TEXT = "hello-from-the-tfs-disk"
# What user64/fileio64.c writes into 'zring3'
RING3 = "ThreadOS 64: a file written from ring 3"


def answer(q, cmd, prefix, timeout=30):
    """Types a command and hands back what arrived after it, once the
    line holding 'prefix' has come in WHOLE. The serial log fills a
    character at a time: a wait for '[core] done, exit code' alone can
    return before the code itself - the part the check reads - is
    there."""
    start = len(q.serial())
    q.type_line(cmd)
    line = re.compile(re.escape(prefix) + r"[^\n]*\n")
    end = time.time() + timeout
    while time.time() < end:
        fresh = q.serial()[start:]
        if line.search(fresh):
            return fresh
        time.sleep(0.3)
    return q.serial()[start:]


def listing(q):
    """The root directory as 'fls' prints it: the names, one a line"""
    out = q.command("fls", "Used:", timeout=30)
    return [l.split()[0] for l in out.replace("\r", "").splitlines()
            if l.startswith("  ") and l.strip()]


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        q.wait_serial(PROMPT, timeout=40)

        # --- The file system on the disk (TFS) ---
        # A ring 3 process writes and reads a file with syscalls
        out = answer(q, "run fileio64", "[fileio64] done, exit code",
                     timeout=60)
        check("a ring 3 process wrote and read a file off the disk",
              "written and read back match byte for byte" in out and
              "[fileio64] done, exit code 0" in out)

        # The kernel's shell: write -> cat (a write and a read within one boot)
        out = answer(q, "write greeting " + TEXT, "write:")
        check("writing a file from the kernel's shell",
              "write: 'greeting' written" in out)
        check("reading the written file back (cat)",
              TEXT in q.command("cat greeting", TEXT, timeout=30))

        # fls shows both files
        names = listing(q)
        check("fls lists the files on the disk (%s)" % names,
              "greeting" in names and "zring3" in names)

        # cat of a file that does not exist
        check("cat reports that the file is missing",
              "cat: there is no file 'nofile'" in
              answer(q, "cat nofile", "cat:"))

        # A 'cat' program in ring 3: it reads a file through an fd and
        # writes to fd 1 (a unified I/O model: read the file -> write to
        # the console)
        out = answer(q, "run ucat64 greeting", "[ucat64] done, exit code",
                     timeout=60)
        check("the ring 3 'cat' reads a file through an fd and writes to fd 1",
              TEXT in out and "[ucat64] done, exit code 0" in out)

        # The second console, while the machine still runs: it shows
        # itself and its own shell answers. That is what makes its
        # silence after the crash say the machine stopped, and not
        # merely that the shell which ran 'crash' is gone. Every console
        # writes to the serial port, so the answer there does not say
        # which shell gave it; the screen does, because a console draws
        # only while it is the one shown (kernel/vga.c), and the first
        # has had no 'uptime' typed on it.
        q.sendkey("alt-f2")
        q.wait_fbcon("console 2", timeout=15)
        out = q.command("uptime", "Uptime:", timeout=30)
        shown = q.wait_fbcon("Uptime:", timeout=15)
        check("before the crash Alt+F2 shows the second console, and its "
              "own shell answers on it",
              "console 2" in shown and "Uptime:" in shown and
              "Uptime:" in out)
        # Polled until the whole of the first console is back: a screen
        # read in the middle of the redraw holds rows of both
        q.sendkey("alt-f1")
        end = time.time() + 15
        shown = q.screen_fbcon()
        while time.time() < end and ("console 2" in shown or
                                     "Uptime:" in shown or
                                     PROMPT not in shown):
            time.sleep(0.4)
            shown = q.screen_fbcon()
        check("and Alt+F1 brings the first one back, where 'crash' is "
              "typed, without the second one's answer",
              "console 2" not in shown and "Uptime:" not in shown and
              PROMPT in shown)

        # At the end: crash should cause a page fault with a diagnostic
        out = q.command("crash", "The system has stopped",
                        timeout=30).replace("\r", "")
        check("the page fault was served (exception 14)",
              "CPU EXCEPTION 14: Page Fault" in out)
        # 'crash' writes to 0x00007E0000000000; the report gives CR2 as
        # its high and low 32 bits
        check("CR2 points at the faulty address",
              "Page fault at the address (CR2): 7e00:0\n" in out)
        check("the system stopped after the exception",
              "The system has stopped" in out)
        # Stopped means stopped: the shell no longer answers
        check("and nothing answers a command after it",
              "Uptime:" not in q.command("uptime", "Uptime:", timeout=8))
        # Nor does any other shell. Had the fault killed only the task
        # that ran 'crash', the second console would answer as it did
        # before.
        q.sendkey("alt-f2")
        check("not even the second console, which answered before the crash",
              "Uptime:" not in q.command("uptime", "Uptime:", timeout=8))
    finally:
        q.close()

    # --- Persistence: a second boot on THE SAME disk ---
    print("--- a second boot (the same disk) ---")
    q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        boot = q2.wait_serial(PROMPT, timeout=40)
        check("the second boot: the disk was found"
              " (already formatted, no second format)",
              "+ TFS ready" in boot and "formatted" not in boot)

        names = listing(q2)
        check("the files survived the restart (fls)",
              "greeting" in names and "zring3" in names)

        check("the file's content survived the restart (cat)",
              TEXT in q2.command("cat greeting", TEXT, timeout=30))

        check("the file written by ring 3 survived the restart",
              RING3 in q2.command("cat zring3", RING3, timeout=30))

        # --- a new name, without moving a single block ------------------
        # A rename changes one entry on the disk: the blocks of a file
        # do not move when it is renamed.
        out = answer(q2, "run core mv greeting welcome",
                     "[core] done, exit code", timeout=60)
        # 'usage:', 'mv: no permission' and 'mv: cannot move' are every
        # word c_mv (user/core.c) has to say, and all of them mean the
        # rename did not happen
        check("mv says nothing and ends with no error",
              "mv:" not in out and "usage:" not in out and
              "[core] done, exit code 0" in out)
        names = listing(q2)
        check("mv gives a new name, and the old one goes (%s)" % names,
              "welcome" in names and "greeting" not in names)

        check("and the content is the same - the blocks did not move",
              TEXT in q2.command("cat welcome", TEXT, timeout=30))

        # The rules a rename has: the name must be free, and the source
        # must exist. A rename that overwrites is a delete nobody asked
        # for.
        out = answer(q2, "run core mv welcome zring3",
                     "[core] done, exit code", timeout=60)
        check("it does not allow a name that is taken",
              "mv: cannot move 'welcome' to 'zring3'" in out and
              "[core] done, exit code 1" in out)

        out = answer(q2, "run core mv nosuchfile whatever",
                     "[core] done, exit code", timeout=60)
        check("and it does not rename something that is not there",
              "mv: cannot move 'nosuchfile' to 'whatever'" in out and
              "[core] done, exit code 1" in out)

        names = listing(q2)
        check("after both refusals the files are as they were (%s)" % names,
              "welcome" in names and "zring3" in names and
              "whatever" not in names)
        check("and the file whose name was wanted still holds its own content",
              RING3 in q2.command("cat zring3", RING3, timeout=30))
    finally:
        q2.close()
        if os.path.exists(disk):
            os.unlink(disk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
