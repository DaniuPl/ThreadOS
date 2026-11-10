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

Along the way we check here the other area that grows under your
fingers: the stack. A program gets four pages, and a recursion goes
considerably deeper.

And the anonymous memory next to the files: a page that comes from the
heap or from the mapping area has to come zeroed, even when the frame
under it was somebody's a moment ago.
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

# user/deep.c: how many calls deep, every one with a 4 KiB frame of its own
DEEP_LEVELS = define("user/deep.c", "DEEP")
# What a program's stack starts with. The frames span at least
# DEEP_LEVELS pages, so the kernel has to add at least this many fewer
# than that.
STACK_START_PAGES = define("kernel64/elf64.h", "USER64_STACK_PAGES")
# How many pages heap64 checks after going round the machine's memory,
# on the heap and in the mapping area, and how many more of each it has
# the kernel bring in by reading into them
TOUCH = define("user64/heap64.c", "TOUCH")
MAP_TOUCH = define("user64/heap64.c", "MAP_TOUCH")
BY_KERNEL = define("user64/heap64.c", "BY_KERNEL")

# The shell's report on a program's end, as kernel64/shell64.c prints it
REPORT = (r"\[%s\] done, exit code (-?\d+) \(frames (\d+) -> (\d+), "
          r"heap pages (\d+), stack (\d+), from files (\d+)\)\r?\n")


def run(q, line, timeout):
    """Runs a program from the kernel's shell and hands back what arrived
    on the port after the command - once the shell's whole report on the
    program's end is there, or the shell said there is no such program,
    or the time ran out. Waiting for the report's first words is not
    enough: the numbers the checks read follow them on the same line."""
    prog = line.split()[1]
    start = len(q.serial())
    q.type_line(line)
    end = time.time() + timeout
    while time.time() < end:
        fresh = q.serial()[start:]
        if (re.search(REPORT % re.escape(prog), fresh)
                or "run: there is no program" in fresh):
            return fresh
        time.sleep(0.3)
    return q.serial()[start:]


def pages_from_files(text):
    """How many pages the kernel read from mapped files - the shell gives
    that in the summary after every program."""
    m = re.search(r"from files (\d+)\)", text)
    return int(m.group(1)) if m else None


def main():
    print("--- files mapped into memory ---")

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
        check("the disc with the kernel builds", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True)
        try:
            q.wait_serial("thread64>", timeout=90)
            q.wait_serial("TFS ready", timeout=30)

            # --- A small file: read whole through a mapping ---
            make_it = run(q, "run mkmap64 map.txt 12", timeout=60)
            check("a file with the pattern was created on the disk",
                  "[mkmap64] wrote 12288 B of the pattern" in make_it)

            read_out = run(q, "run mmapfile64 map.txt", timeout=90)
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
            run(q, "run mkmap64 big.txt 400", timeout=120)
            one_byte = run(q, "run mmapfile64 big.txt 1", timeout=90)
            check("mapping a big file works",
                  "I never touched the rest of the file" in one_byte)
            check("touching one byte costs one page, "
                  "and not the whole file", pages_from_files(one_byte) == 1)

            # ...and reading the whole of it costs as much as it has pages
            many = run(q, "run mmapfile64 big.txt", timeout=120)
            check("reading the whole file pulls in every one of its pages",
                  pages_from_files(many) == 400 * 1024 // 4096)

            # A mapping does not outlive the process: the memory returns
            # to the pool
            m = re.search(r"mmapfile64\] done, exit code 0 "
                          r"\(frames (\d+) -> (\d+)", many)
            check("once the program ended the frames came back to the pool",
                  m is not None and m.group(1) == m.group(2))

            # A file that does not exist is an error, not an empty address
            missing = run(q, "run mmapfile64 nosuchthing", timeout=60)
            check("a file that is not there is reported as an error",
                  "the file cannot be mapped" in missing)

            # --- The stack grows under your fingers too ---
            # A program gets four pages, and the recursion goes far
            # deeper - and on the way back it checks whether every frame
            # is still its own. Adding pages on top of overwriting
            # somebody else's frames would look the same halfway.
            stack = run(q, "run deep", timeout=90)
            check("the stack grows on demand: the recursion goes past "
                  "%d pages" % STACK_START_PAGES,
                  "[deep] I went down to level %d" % DEEP_LEVELS in stack)
            check("and every frame survived untouched",
                  "every frame whole" in stack and "WRONG" not in stack)
            # The shell counts the pages the kernel added to the stack.
            # Without that the recursion could have fitted into what the
            # program was given at the start, and the test would prove
            # nothing about growing
            m = re.search(REPORT % "deep", stack)
            check("the kernel added the pages the recursion needed "
                  "(%s, at least %d)"
                  % (m.group(5) if m else "?",
                     DEEP_LEVELS - STACK_START_PAGES),
                  m is not None and m.group(1) == "0" and
                  int(m.group(5)) >= DEEP_LEVELS - STACK_START_PAGES)

            # --- Fresh anonymous pages are zeroed ---
            # The kernel hands frames out going round in a circle, so on
            # a machine this young the frames ahead have never held
            # anything, and a page that was never cleared would still
            # read as zeros. heap64 goes round first - takes, dirties
            # and gives back more pages than the machine has frames -
            # and only then checks a few fresh pages of the heap and of
            # the mapping area: their frames are the ones it dirtied.
            reuse = run(q, "run heap64 reuse", timeout=240)
            m = re.search(r"went round: (\d+) pages taken, dirtied and "
                          r"given back; the machine has (\d+) frames",
                          reuse)
            cycled = int(m.group(1)) if m else 0
            check("heap64 went round more pages than the machine has frames"
                  + (" (%s of %s)" % m.groups() if m else ""),
                  m is not None and cycled > int(m.group(2)) > 0)
            # The kernel's own count: every page went through its
            # hands, one fresh frame each, and then the few checked
            r = re.search(REPORT % "heap64", reuse)
            check("the kernel handed out a fresh frame for each of them"
                  + (" (%s heap pages)" % r.group(4) if r else ""),
                  r is not None
                  and int(r.group(4)) == cycled + TOUCH + BY_KERNEL)
            check("heap pages on frames used and freed before read as "
                  "zeros",
                  "[heap64] the %d heap pages read as zeros before I wrote "
                  "to them" % TOUCH in reuse)
            check("so do pages of the mapping area",
                  "touched %d - zeros at first, then what I wrote"
                  % MAP_TOUCH in reuse and r is not None
                  and int(r.group(6)) == MAP_TOUCH + BY_KERNEL)
            # The other way a page comes in, with a clearing of its own:
            # not the program's fault on its first touch, but the kernel
            # checking the buffer of a read() into a page nobody has
            # touched. Leaving the clearing out there alone would pass
            # everything above.
            check("a heap page the kernel brings in for a read() is zeros "
                  "around what was read",
                  "[heap64] a page the kernel brought in on the heap: "
                  "zeros around what it read" in reuse)
            check("and so is one in the mapping area",
                  "[heap64] a page the kernel brought in in the mapping "
                  "area: zeros around what it read" in reuse)
            check("heap64 ends with no error",
                  r is not None and r.group(1) == "0"
                  and "[heap64] ERROR" not in reuse)
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, disk):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
