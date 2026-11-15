#!/usr/bin/env python3
"""ThreadOS smoke test: ring 3 processes and their memory.

A user program is a separate ELF file with its own address space. This
group walks in turn through what such a program gets from the kernel:
system calls, arguments, a growing heap, threads sharing its memory,
anonymous mappings and memory shared with another process.

After EVERY program the kernel shell says how many frames were taken
before it started and after it ended. A process that has finished is to
give everything back - that is the only assertion that catches a memory
leak, because the program itself will say nothing about it. At the end
'mem' is asked as well, and has to agree.

Everything is read from the serial port: GRUB gives the kernel a
framebuffer, so the VGA character memory says nothing.

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


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    slog = tempfile.mktemp(suffix=".log")

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
    try:
        q.wait_serial(PROMPT, timeout=90)
        q.wait_serial("[net] DHCP", timeout=30)
        time.sleep(0.5)

        def frames():
            m = re.search(r"frames (\d+)/\d+ used",
                          q.command("mem", PROMPT, timeout=20))
            return int(m.group(1)) if m else None

        def run(line, timeout=60):
            """Runs a program; what it said, its exit code and whether
            the frames it took came back"""
            name = line.split()[0]
            # Over once the shell prompts again: it waits for its child
            out = q.command("run " + line, PROMPT, timeout=timeout)
            out = out.replace("\r", "")
            m = re.search(r"\[%s\] done, exit code (-?\d+) "
                          r"\(frames (\d+) -> (\d+)" % name, out)
            code = int(m.group(1)) if m else None
            leak = (f"{m.group(2)} -> {m.group(3)}" if m else "no line")
            kept = m is not None and int(m.group(3)) <= int(m.group(2))
            return out, code, leak, kept

        used_before = frames()

        # An ELF process with its own address space, and system calls
        out, code, leak, kept = run("hello64")
        check("hello64 started from an ELF file and makes a write syscall",
              "[hello64] hello, I am a 64-bit ELF" in out)
        # User space starts from 1 GiB - below there is only the
        # kernel's mapping
        m = re.search(r"Program 'hello64' from initrd: \d+ B, "
                      r"entry ([0-9a-f]+)", out)
        check("the user program lives above 1 GiB"
              + (f" (entry {m.group(1)})" if m else ""),
              m is not None and int(m.group(1), 16) >= 0x40000000)
        check("the write and sleep syscalls work in a loop",
              "[hello64] counting: 3" in out)
        check("hello64 ended through sys_exit with its own code (7)",
              code == 7)
        check(f"no frames leaked after the process ({leak})", kept)

        # The arguments reach the process: the program's own line, not
        # the echo of the typed command
        out, code, leak, kept = run("uecho64 thread os works")
        check("the argc/argv arguments reach the process (uecho64)",
              re.search(r"^thread os works$", out, re.M) is not None
              and code == 0)

        # A heap that grows as the program asks
        out, code, leak, kept = run("heap64")
        check("sbrk works: the heap grows, and gives back what was written",
              "every one gave back what I wrote" in out and code == 0)
        check(f"no frames leaked after the heap ({leak})", kept)

        # Threads in one process share its memory: with a mutex, their
        # increments of one counter all land
        out, code, leak, kept = run("mutex")
        m = re.search(r"with the mutex=(\d+) \(expected (\d+)\)", out)
        check("threads in a process share its memory"
              + (f" ({m.group(1)} of {m.group(2)})" if m else ""),
              m is not None and m.group(1) == m.group(2) and code == 0)
        check(f"no frames leaked after the threads ({leak})", kept)

        # Anonymous mappings, written by several threads at once, and
        # then unmapped
        out, code, leak, kept = run("tlbtest64", timeout=90)
        check("mmap gives memory that several threads write and agree on",
              "errors while writing 0, afterwards 0" in out and code == 0)
        check(f"no frames leaked after the mappings ({leak})", kept)

        # Shared memory between processes: the writer starts the reader,
        # a separate process, which reads the same memory
        out, code, leak, kept = run("shmwrite64")
        check("shared memory: another process reads the written value",
              "[shmread64] value=12345" in out and code == 0)

        check(f"no frames leaked after the shared memory ({leak})", kept)

        # And 'mem' agrees with the bills above
        used_after = frames()
        check(f"'mem' agrees: nothing was left behind"
              f" ({used_before} -> {used_after})",
              used_before is not None and used_after is not None
              and used_after <= used_before)
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
