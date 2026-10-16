#!/usr/bin/env python3
"""ThreadOS smoke test: files mapped into memory.

The ELF loader and the file system we joined earlier - a program can lie
on the disk. This is the other half of the same thought: a file can be
MEMORY. The program gets an address and reads the contents like an
ordinary array, without a single read call.

We check three things, each a different way:

  1. the bytes at the address match what lies in the file,
  2. the pages arrive UNDER YOUR FINGERS - touching one byte of a
     hundred-page file is to cost one page, not a hundred,
  3. the mapping is read-only and an attempted write is to kill the
     process rather than quietly vanish.

The kernel works the block numbers out once, when the mapping is set up,
and reads them afterwards through the block layer, bypassing the TFS
code - for the same reason as with page swapping: TFS has one shared
buffer for a block, and a page fault can land in the middle of any file
operation.

Both kernels have the same thing, each with its own paging code - and we
check both the same way. Along the way we check here the other area that
grows under your fingers: the stack. A program gets eight pages, and a
recursion goes considerably deeper.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def pages_from_files(text):
    """How many pages the kernel read from mapped files - the shell gives
    that in the summary after every program."""
    m = re.search(r"from files (\d+)\)", text)
    return int(m.group(1)) if m else None


def main():
    print("--- files mapped into memory (the 64-bit kernel) ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return 0

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
            q.wait_serial("thread64>", timeout=90)
            q.wait_serial("TFS ready", timeout=30)

            # --- A small file: read whole through a mapping ---
            make_it = q.command("run mkmap64 map.txt 12", "exit code",
                               timeout=60)
            check("a file with the pattern was created on the disk",
                  "[mkmap64] wrote 12288 B of the pattern" in make_it)

            read_out = q.command("run mmapfile64 map.txt", "exit code",
                               timeout=90)
            addr = re.search(r"the file at address (\d+)", read_out)
            check("the file got an address in the mapping area",
                  addr is not None and
                  0x50000000 <= int(addr.group(1)) < 0x60000000)
            check("every byte of the file matches the pattern",
                  "the checked bytes match, and there were 12288" in read_out)
            check("trying to write to the mapping kills the process",
                  "writing to the mapping was refused" in read_out and
                  "killed: Page Fault" in read_out)
            check("the file's content after the refused write is untouched",
                  "WRONG" not in read_out)
            check("the file was read without a single read call",
                  "without a single read" in read_out)
            check("twelve kilobytes are exactly three pages off the disk",
                  pages_from_files(read_out) == 3)

            # --- A large file, one byte touched ---
            # This is the real proof of paging on demand: a mapping
            # costs as much as was taken from it
            q.command("run mkmap64 big.txt 400", "exit code",
                        timeout=120)
            malo = q.command("run mmapfile64 big.txt 1", "exit code",
                               timeout=90)
            check("mapping a big file works",
                  "I never touched the rest of the file" in malo)
            check("touching one byte costs one page, "
                  "and not the whole file", pages_from_files(malo) == 1)

            # ...and reading the whole of it costs as much as it has pages
            many = q.command("run mmapfile64 big.txt", "exit code",
                               timeout=120)
            check("reading the whole file pulls in every one of its pages",
                  pages_from_files(many) == 400 * 1024 // 4096)

            # A mapping does not outlive the process: the memory returns to the
# pool
            m = re.search(r"mmapfile64\] done, exit code 0 "
                          r"\(frames (\d+) -> (\d+)", many)
            check("once the program ended the frames came back to the pool",
                  m is not None and m.group(1) == m.group(2))

            # A file that does not exist is an error, not an empty address
            missing = q.command("run mmapfile64 nosuchthing",
                               "exit code", timeout=60)
            check("a file that is not there is reported as an error",
                  "the file cannot be mapped" in missing)
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, disk):
            if os.path.exists(f):
                os.unlink(f)

    # --- The same in the 32-bit kernel ---
    print("--- files mapped into memory (the 32-bit kernel) ---")
    disk32 = tempfile.mktemp(suffix=".img")
    slog32 = tempfile.mktemp(suffix=".log")
    with open(disk32, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        q3 = Qemu(KERNEL, INITRD, disk32, append=TEXT_MODE, serial=slog32)
        try:
            q3.wait_for("ThreadOS shell", timeout=40)
            q3.wait_for("[net] DHCP", timeout=30)

            make_it = q3.command("run mkmap map.txt 12", "has ended",
                                timeout=60)
            check("a file with the pattern was created on the disk (32-bit)",
                  "[mkmap] wrote 12288 B of the pattern" in make_it)

            read_out = q3.command("run mmapfile map.txt", "has ended",
                                timeout=90)
            addr = re.search(r"the file at address (\d+)", read_out)
            check("the file got an address in the mmap area (32-bit)",
                  addr is not None and
                  0xB0000000 <= int(addr.group(1)) < 0xBF000000)
            check("every byte of the file matches the pattern (32-bit)",
                  "the checked bytes match, and there were 12288" in read_out)
            check("trying to write to the mapping kills the process (32-bit)",
                  "writing to the mapping was refused" in read_out and
                  re.search(r"killed on core \d+: Page Fault", read_out) is not None)
            check("the file's content after the refused write is untouched (32-bit)",
                  "WRONG" not in read_out)

            # Paging on demand: a large file, one byte touched
            before = q3.command("mem", "from files:")
            m1 = re.search(r"from files:\s+(\d+)", before)
            q3.command("run mkmap big.txt 400", "has ended",
                         timeout=120)
            malo = q3.command("run mmapfile big.txt 1", "has ended",
                                timeout=90)
            check("mapping a big file works (32-bit)",
                  "I never touched the rest of the file" in malo)
            after = q3.command("mem", "from files:")
            m2 = re.search(r"from files:\s+(\d+)", after)
            check("touching one byte costs one page (32-bit)",
                  m1 is not None and m2 is not None and
                  int(m2.group(1)) - int(m1.group(1)) == 1)

            # The stack grows under your fingers too. A program gets eight
            # pages, and a recursion goes far deeper - and on the way
            # back it checks whether every frame is still its own.
            # Adding pages on top of overwriting somebody else's frames
            # would look the same halfway.
            stack = q3.command("run deep", "has ended", timeout=60)
            check("the stack grows on demand: the recursion goes past eight "
                  "pages", "[deep] I went down to level 40" in stack)
            check("and every frame survived untouched",
                  "every frame whole" in stack and "WRONG" not in stack)
        finally:
            q3.close()
    finally:
        for f in (disk32, slog32):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
