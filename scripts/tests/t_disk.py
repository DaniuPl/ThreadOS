#!/usr/bin/env python3
"""ProtonOS smoke test: running programs from the disk.

The ELF loader and the file system existed side by side for a long time
and were never joined: 'run' looked only into the initrd, that is, into
the image uploaded together with the kernel and read-only. Now a program
can lie on the disk - and can still be run.

The strongest proof is the same as with the file system: a SECOND BOOT of
the same machine with the same disk image. A program that starts after a
restart, and that is not in the initrd, can only have come off the
platter.

Looking for a program (kernel/prog.c) is shared by both kernels - there
is not a word about the processor mode in it - so we check it on both
sides.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64

# A name that is NOT in the initrd - otherwise there is no telling
# where the program came from
ON_DISK = "fromdisk"


def main():
    print("--- programs from the disk (the 64-bit kernel) ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        check("the disc with the 64-bit kernel builds", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        # --- The first boot: the program goes from the initrd onto the disk ---
        slog = tempfile.mktemp(suffix=".log")
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, qemu="qemu-system-x86_64")
        try:
            q.wait_serial("PFS ready", timeout=90)
            q.wait_serial("proton64>", timeout=30)

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
            if os.path.exists(slog):
                os.unlink(slog)

        # --- A second boot with the same disk ---
        slog2 = tempfile.mktemp(suffix=".log")
        q2 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog2, disk=disk,
                  sata=True, qemu="qemu-system-x86_64")
        try:
            q2.wait_serial("proton64>", timeout=90)
            po = q2.command("run " + ON_DISK, "[hello64] ending",
                              timeout=30)
            check("the program survived the restart and still starts",
                  "Program '%s' from the disk" % ON_DISK in po)

            # A program from the disk is an ordinary program: it has its
            # own address space and ends with its own exit code
            check("a program from the disk ends with its own exit code",
                  "done, exit code 7" in po)

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
            if os.path.exists(slog2):
                os.unlink(slog2)
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, disk):
            if os.path.exists(f):
                os.unlink(f)

    # --- The same in the 32-bit kernel ---
    # The same file looking for the program, the same path; the only
    # difference is the ELF loader on the other side
    print("--- programs from the disk (the 32-bit kernel) ---")
    disk32 = tempfile.mktemp(suffix=".img")
    slog32 = tempfile.mktemp(suffix=".log")
    with open(disk32, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        q3 = Qemu(KERNEL, INITRD, disk32, append=TEXT_MODE, serial=slog32)
        try:
            q3.wait_for("ProtonOS shell", timeout=40)
            copied = q3.command("cp hello " + ON_DISK, "cp:")
            check("'cp' copies a program onto the disk (32-bit)",
                  "cp: 'hello' -> '%s'" % ON_DISK in copied)
            wyn = q3.command("run " + ON_DISK, "[hello] ending",
                               timeout=30)
            check("a program from the disk starts (32-bit)",
                  "[hello] hello" in wyn)
            check("the 32-bit kernel reports where it took it from too",
                  "exec '%s' from the disk" % ON_DISK in wyn)
        finally:
            q3.close()
    finally:
        for f in (disk32, slog32):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
