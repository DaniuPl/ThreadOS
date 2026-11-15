#!/usr/bin/env python3
"""ThreadOS smoke test: the kernel shell's commands.

The kernel's shell is the only way into the system until a ring 3 shell
starts - and the first thing to stop working when something breaks
deeper down. We check that it prints what it should and that the numbers
it shows agree with reality.

The strongest assertion here is the one about managed memory: the
identity mapping is to reach all the way to the top of the detected RAM,
not to the first 64 MiB.

Everything is read from the serial port: GRUB hands the kernel a
framebuffer even in text mode, so the VGA character memory stays empty,
and the port also keeps what has scrolled off the screen.

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
        # The DHCP message falls into the middle of a line being typed, so
        # we wait for it before we start typing
        q.wait_serial("[net] DHCP", timeout=30)
        time.sleep(0.5)

        # A command is over when the next prompt comes - the shell
        # prints it only after the command has returned
        out = q.command("help", PROMPT, timeout=20)
        check("the 'help' command prints the list of commands",
              "Commands:" in out and "reboot" in out and "run <program>"
              in out)

        mem = q.command("mem", PROMPT, timeout=20)
        check("the 'mem' command shows the RAM and the frames",
              re.search(r"Memory: \d+ KiB RAM, frames \d+/\d+ used", mem)
              is not None)
        # All the detected memory is under management, not only the first
        # 64 MiB: the identity mapping stretches all the way to the top of
        # the RAM.
        mm = re.search(r"identity-mapped (\d+) MiB", mem)
        ram = re.search(r"Memory: (\d+) KiB RAM", mem)
        check("the managed memory reaches the top of the RAM found"
              + (f" ({mm.group(1)} MiB of {int(ram.group(1)) // 1024} MiB)"
                 if mm and ram else ""),
              mm is not None and ram is not None
              and int(mm.group(1)) >= int(ram.group(1)) // 1024 - 2)

        # Two looks, seconds apart: a clock that does not move would
        # still print a line
        def uptime():
            m = re.search(r"Uptime: (\d+) s \((\d+) ticks",
                          q.command("uptime", "ticks", timeout=20))
            return int(m.group(2)) if m else None
        t1 = uptime()
        time.sleep(2)
        t2 = uptime()
        check(f"the 'uptime' command works, and the ticks move ({t1} -> {t2})",
              t1 is not None and t2 is not None and t2 >= t1 + 150)

        # The short-lived thread started at boot ends on its own and is
        # then reaped. How soon that happens depends on how loaded the
        # host is, so we look again rather than assume it has happened.
        for _ in range(20):
            ps = q.command("ps", PROMPT, timeout=20)
            if not re.search(r"\bshort\b", ps):
                break
            time.sleep(1.0)
        check("the 'ps' command shows the threads",
              re.search(r"\bmain\b", ps) is not None and "count-A" in ps
              and "count-B" in ps and "idle" in ps)
        check("the shell is a thread of its own (not an interrupt context)",
              "shell (current)" in ps)
        check("the thread that ended was cleaned up, not left a zombie",
              re.search(r"\bshort\b", ps) is None and "zombie" not in ps)

        # initrd: 'ls' shows the packed programs. We ask about the SHAPE
        # of the list rather than about every name: every line is a name
        # and a non-zero size, there are dozens of lines, and among them
        # familiar programs.
        ls = q.command("ls", PROMPT, timeout=20)
        entries = re.findall(r"^  (\w+)  \((\d+) B\)", ls.replace("\r", ""),
                             re.M)
        known = {"hello64", "fork64", "spawn64", "sigtest64", "swaptest64",
                 "mkmap64", "mmapfile64", "mmapzap64", "dynlib", "ush64"}
        names = {n for n, _ in entries}
        check("the 'ls' command shows the initrd files (%d entries, %d known)"
              % (len(entries), len(names & known)),
              len(entries) >= 15
              and all(int(size) > 0 for _, size in entries)
              and names >= known)

        miss = q.command("run nothing", PROMPT, timeout=20)
        check("run reports the file is missing - and says where it looked",
              "there is no program 'nothing' (see 'ls' and 'fls')" in miss)
        bad = q.command("nosuchcommand", PROMPT, timeout=20)
        check("an unknown command is reported, not ignored",
              "Unknown command: nosuchcommand" in bad)
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
