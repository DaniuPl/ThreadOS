#!/usr/bin/env python3
"""ProtonOS smoke test: the PFS file system on the disk.

A file written by a ring 3 process and a file written from the kernel's
shell are to be the same file seen by both - and both are to survive a
restart of the machine. That last part matters most here: everything
else can be faked with a cache, and a SECOND BOOT on the same disk image
cannot.

At the end of the first boot we call 'crash', that is, a deliberate page
fault in the kernel. The diagnostic is to give the exception number and
the address from CR2, and the system is to stop rather than blunder on.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
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

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        q.wait_for("ProtonOS shell", timeout=40)

        # --- The file system on the disk (PFS) ---
        # A ring 3 process writes and reads a file with syscalls
        q.type_line("run fileio")
        screen = q.wait_for("[fileio] OK")
        check("a ring 3 process wrote and read a file off the disk",
              "read off the disk: a file written by a ring 3 process" in screen)

        # The kernel's shell: write -> cat (a write and a read within one boot)
        q.type_line("write greeting hello-from-the-pfs-disk")
        check("writing a file from the kernel's shell",
              "wrote" in q.wait_for("wrote"))
        q.type_line("cat greeting")
        check("reading the written file back (cat)",
              "hello-from-the-pfs-disk" in q.wait_for("hello-from-the-pfs-disk"))

        # fls shows both files
        q.type_line("fls")
        screen = q.wait_for("Root directory")
        check("fls lists the files on the disk",
              "greeting" in screen and "ring3.txt" in screen)

        # cat of a file that does not exist
        q.type_line("cat nofile")
        check("cat reports that the file is missing",
              "there is no file 'nofile'" in q.wait_for("there is no file"))

        # The 'cat' program in ring 3: it reads a file through an fd and
        # writes to fd 1 (a unified I/O model: read the file -> write to
        # the console). We clear the screen so as not to catch the old
        # echo of 'hello-from-the-pfs-disk'.
        q.type_line("clear")
        time.sleep(0.3)
        q.type_line("run cat greeting")
        check("the ring 3 'cat' reads a file through an fd and writes to fd 1",
              "hello-from-the-pfs-disk" in q.wait_for("hello-from-the-pfs-disk",
                                                timeout=20))

        # At the end: crash should cause a page fault with a diagnostic
        q.type_line("crash")
        screen = q.screen()
        check("the page fault was served (exception 14)",
              "CPU EXCEPTION 14: Page Fault" in screen)
        check("CR2 points at the faulty address",
              "(CR2): e0000000" in screen)
        check("the system stopped after the exception", "The system has stopped" in screen)
    finally:
        q.close()

    # --- Persistence: a second boot on THE SAME disk ---
    print("--- a second boot (the same disk) ---")
    q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
    try:
        screen = q2.wait_for("ProtonOS shell")
        check("the second boot: the disk was found"
              " (already formatted, no second format)",
              "+ PFS ready" in screen and "formatted" not in screen)

        q2.type_line("fls")
        screen = q2.wait_for("Root directory")
        check("the files survived the restart (fls)",
              "greeting" in screen and "ring3.txt" in screen)

        q2.type_line("cat greeting")
        check("the file's content survived the restart (cat)",
              "hello-from-the-pfs-disk" in q2.wait_for("hello-from-the-pfs-disk"))

        q2.type_line("cat ring3.txt")
        check("the file written by ring 3 survived the restart",
              "a file written by a ring 3 process" in q2.wait_for("ring3"))

        # --- a new name, without moving a single block ------------------
        # 'mv' used to read the whole file and write it out again; the
        # blocks of a file do not move when it is renamed, and now
        # neither does the code pretend they do.
        q2.type_line("mv greeting powitanie")
        q2.wait_for("mv:", timeout=20)
        # The screen keeps what scrolled past, and 'greeting' was
        # printed on it a moment ago - so it is wiped before the
        # listing is read, or the old name would be found in the old
        # output rather than in the directory.
        q2.type_line("clear")
        time.sleep(1)
        q2.type_line("fls")
        screen = q2.wait_for("Root directory")
        check("mv gives a new name, and the old one goes",
              "powitanie" in screen and "greeting" not in screen)

        q2.type_line("cat powitanie")
        check("and the content is the same - the blocks did not move",
              "hello-from-the-pfs-disk" in q2.wait_for("hello-from-the-pfs-disk"))

        # The rules a rename has: the name must be free, and the source
        # must exist. A rename that overwrites is a delete nobody asked
        # for.
        q2.type_line("clear")
        time.sleep(1)
        q2.type_line("mv powitanie ring3.txt")
        screen = q2.wait_for("mv:", timeout=20)
        check("it does not allow a name that is taken", "cannot" in screen)

        q2.type_line("clear")
        time.sleep(1)
        q2.type_line("mv nosuchfile whatever")
        screen = q2.wait_for("mv:", timeout=20)
        check("and it does not rename something that is not there",
              "cannot" in screen)

        q2.type_line("clear")
        time.sleep(1)
        q2.type_line("fls")
        screen = q2.wait_for("Root directory")
        check("after both refusals the files are as they were",
              "powitanie" in screen and "ring3.txt" in screen)
    finally:
        q2.close()
        if os.path.exists(disk):
            os.unlink(disk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
