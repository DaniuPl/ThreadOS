#!/usr/bin/env python3
"""ProtonOS smoke test: writing through a file mapped into memory.

Mappings were one-way so far: a file could be read like memory, but not
written. Now the road goes both ways - the program changes bytes at an
address and they land in the file on disk.

Along the way we check four things that are easy to get wrong:

  1. We flush ONLY what changed. The program reads the whole file and
     writes over two pages - and two pages are to go back to the disk.
     Which ones is said by the processor: it sets the "dirty" bit itself
     on a write. A second sys_msync in a row is to flush zero.
  2. The changes can be seen with an ORDINARY read, and after a machine
     restart on the disk too - not only in the memory that held them.
  3. The program does not have to ask: what it did not manage to flush
     itself is flushed by the kernel when the process ends.
  4. A writable mapping after a fork is SHARED, not copied on write. Both
     processes write into one file, so a private copy in the child would
     mean its writes never go beyond its own memory.

Both kernels have the same thing, each with its own paging code - and we
check both the same way.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def main():
    print("--- writing through a mapping (the 64-bit kernel) ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
    else:
        d = tempfile.mkdtemp()
        iso = tempfile.mktemp(suffix=".iso")
        slog = tempfile.mktemp(suffix=".log")
        disk = tempfile.mktemp(suffix=".img")
        with open(disk, "wb") as fh:
            fh.truncate(DISK_SIZE)
        try:
            check("the disc with the 64-bit kernel builds", make_iso64(d, iso))
            if not os.path.exists(iso):
                return summary()

            q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                     sata=True, qemu="qemu-system-x86_64")
            try:
                q.wait_serial("proton64>", timeout=90)
                q.wait_serial("PFS ready", timeout=30)

                make_it = q.command("run mkmap64 written.txt 12", "exit code",
                                   timeout=60)
                check("a file with the pattern was created on the disk",
                      "[mkmap64] wrote 12288 B of the pattern" in make_it)

                write_it = q.command("run mmapzap64 written.txt write",
                                   "exit code", timeout=90)
                check("the whole file can be read through the mapping "
                      "for writing",
                      "the old pattern was read, bytes 12288" in write_it)
                check("only the pages written to are sent back",
                      "pages sent back: 2" in write_it)
                check("a page that was sent back stops being dirty",
                      "the second msync sent back: 0" in write_it)
                check("sending it to the disk does not touch the content in memory",
                      "writing through memory is done" in write_it and "WRONG" not in write_it)

                probe = q.command("run mmapzap64 written.txt check",
                                  "exit code", timeout=90)
                check("an ordinary read sees what the memory wrote",
                      "an ordinary read sees what" in probe and "WRONG" not in probe)

                # Without an explicit msync - the kernel flushes it at the end of the
# process
                q.command("run mkmap64 nosync.txt 12", "exit code",
                            timeout=60)
                without = q.command("run mmapzap64 nosync.txt nosync",
                                  "exit code", timeout=60)
                check("a program may end without asking for a flush",
                      "ending with no msync" in without)
                probe2 = q.command("run mmapzap64 nosync.txt check2",
                                   "exit code", timeout=60)
                check("the kernel sends dirty pages back as the process ends",
                      "reached the disk as the process ended" in probe2)

                # After a fork the mapping is shared, not copied
                q.command("run mkmap64 shared.txt 12", "exit code",
                            timeout=60)
                visible = q.command("run mmapzap64 shared.txt shared",
                                    "exit code", timeout=60)
                check("the parent sees the child's write - the mapping is shared",
                      "the mapping is shared" in visible and "WRONG" not in visible)
            finally:
                q.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)
            for f in (iso, slog, disk):
                if os.path.exists(f):
                    os.unlink(f)

    # --- The same in the 32-bit kernel, with a restart at the end ---
    print("--- writing through a mapping (the 32-bit kernel) ---")
    disk32 = tempfile.mktemp(suffix=".img")
    slog32 = tempfile.mktemp(suffix=".log")
    slog4 = tempfile.mktemp(suffix=".log")   # the second boot writes to its own
    with open(disk32, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        q3 = Qemu(KERNEL, INITRD, disk32, append=TEXT_MODE, serial=slog32)
        try:
            q3.wait_for("ProtonOS shell", timeout=40)
            q3.wait_for("[net] DHCP", timeout=30)

            make_it = q3.command("run mkmap written.txt 12", "has ended",
                                timeout=60)
            check("a file with the pattern was created on the disk (32-bit)",
                  "[mkmap] wrote 12288 B of the pattern" in make_it)

            write_it = q3.command("run mmapzap written.txt write", "has ended",
                                timeout=90)
            check("the whole file can be read through the mapping "
                  "for writing (32-bit)",
                  "the old pattern was read, bytes 12288" in write_it)
            check("only the pages written to are sent back (32-bit)",
                  "pages sent back: 2" in write_it)
            check("a page that was sent back stops being dirty (32-bit)",
                  "the second msync sent back: 0" in write_it)
            check("sending it to the disk does not touch the content in memory (32-bit)",
                  "writing through memory is done" in write_it and "WRONG" not in write_it)

            probe = q3.command("run mmapzap written.txt check", "has ended",
                               timeout=90)
            check("an ordinary read sees what the memory wrote (32-bit)",
                  "an ordinary read sees what" in probe and "WRONG" not in probe)

            q3.command("run mkmap nosync.txt 12", "has ended",
                         timeout=60)
            without = q3.command("run mmapzap nosync.txt nosync", "has ended",
                               timeout=60)
            check("a program may end without asking for a flush "
                  "(32-bit)", "ending with no msync" in without)
            probe2 = q3.command("run mmapzap nosync.txt check2",
                                "has ended", timeout=60)
            check("the kernel sends dirty pages back as the process ends (32-bit)",
                  "reached the disk as the process ended" in probe2)

            q3.command("run mkmap shared.txt 12", "has ended",
                         timeout=60)
            visible = q3.command("run mmapzap shared.txt shared",
                                 "has ended", timeout=60)
            check("the parent sees the child's write - the mapping is shared "
                  "(32-bit)",
                  "the mapping is shared" in visible and "WRONG" not in visible)
        finally:
            q3.close()

        # The strongest proof: THE SAME disk image, a new kernel, new
        # memory. If the changes sat only in the process's pages, there
        # would be no trace of them here.
        q4 = Qemu(KERNEL, INITRD, disk32, append=TEXT_MODE, serial=slog4)
        try:
            screen_text = q4.wait_for("ProtonOS shell", timeout=40)
            check("a second boot on the same disk (with no formatting)",
                  "+ PFS ready" in screen_text and "formatted" not in screen_text)
            q4.wait_for("[net] DHCP", timeout=30)

            after = q4.command("run mmapzap written.txt check", "has ended",
                              timeout=90)
            check("the writes through memory survived the machine restarting",
                  "an ordinary read sees what" in after and "WRONG" not in after)
            po2 = q4.command("run mmapzap nosync.txt check2",
                               "has ended", timeout=60)
            check("the bytes sent back as the process ended survived the restart too",
                  "reached the disk as the process ended" in po2)
        finally:
            q4.close()
    finally:
        for f in (disk32, slog32, slog4):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
