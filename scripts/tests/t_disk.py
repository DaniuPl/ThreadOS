#!/usr/bin/env python3
"""ThreadOS smoke test: running programs from the disk.

The ELF loader and the file system existed side by side for a long time
and were never joined: 'run' looked only into the initrd, that is, into
the image uploaded together with the kernel and read-only. Now a program
can lie on the disk - and can still be run.

The strongest proof is the same as with the file system: a SECOND BOOT of
the same machine with the same disk image. A program that starts after a
restart, and that is not in the initrd, can only have come off the
platter.

Looking for a program lives in kernel/prog.c, and the same search serves
both doors into it: the kernel shell's 'run' and an exec from ring 3.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64

# A name that is NOT in the initrd - otherwise there is no telling
# where the program came from
ON_DISK = "fromdisk"


def main():
    print("--- programs from the disk ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return 0

    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        # --- The first boot: the program goes from the initrd onto the disk ---
        q = Qemu(KERNEL, INITRD, disk=disk, sata=True, append=TEXT_MODE)
        try:
            q.wait_serial("TFS ready", timeout=90)
            q.wait_serial("thread64>", timeout=30)

            missing = q.command("run " + ON_DISK, "run:")
            check("before the copy the program is not on the disk",
                  "run: there is no program" in missing)

            copied = q.command("cp hello64 " + ON_DISK, "cp:")
            check("'cp' copies a program from the initrd onto the disk",
                  "cp: 'hello64' -> '%s'" % ON_DISK in copied)

            listing = q.command("fls", "  %s  (" % ON_DISK)
            check("the copied program shows in the disk's file list",
                  "  %s  (" % ON_DISK in listing)

            wyn = q.command("run " + ON_DISK, "[hello64] ending",
                              timeout=30)
            check("a program from the disk starts and runs",
                  "[hello64] hello" in wyn)
            check("the kernel reports it took it off the disk, not out of the initrd",
                  "Program '%s' from the disk" % ON_DISK in wyn)
        finally:
            q.close()

        # --- A second boot with the same disk ---
        q2 = Qemu(KERNEL, INITRD, disk=disk, sata=True, append=TEXT_MODE)
        try:
            q2.wait_serial("thread64>", timeout=90)
            # Waited for up to what follows the exit code on the shell's
            # closing line, so that the code itself has arrived whole
            after = q2.command("run " + ON_DISK, " (frames ", timeout=30)
            check("the program survived the restart and still starts",
                  "Program '%s' from the disk" % ON_DISK in after)

            # A program from the disk is an ordinary program: it has its
            # own address space and ends with its own exit code
            check("a program from the disk ends with its own exit code",
                  "[%s] done, exit code 7 (" % ON_DISK in after)

            # ...and a second time too, that is, the buffer after it was
            # given back. We count only what arrived after THIS command:
            # hello64 also starts at boot, so there is more than one of it
            # on the port.
            two = q2.command("run " + ON_DISK, "[hello64] ending",
                               timeout=30)
            check("a second run from the disk passes too",
                  two.count("[hello64] ending") == 1)

            # The same path from ring 3: the user shell does a fork and
            # an exec, and exec looks for a program just as the kernel
            # does - so a program from the disk starts from there too
            q2.command("run ush64", "ush64$")
            from_ring3 = q2.command(ON_DISK, "[hello64] ending", timeout=30)
            check("exec from ring 3 finds the program on the disk too",
                  "[hello64] hello" in from_ring3)
            q2.command("exit", "done, exit code")

            # A name that is nowhere is still an error
            bad_one = q2.command("run nosuchthing", "run: there is no program")
            check("a program that is not there is still an error",
                  "run: there is no program" in bad_one)
        finally:
            q2.close()
    finally:
        if os.path.exists(disk):
            os.unlink(disk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
