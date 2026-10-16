#!/usr/bin/env python3
"""ThreadOS smoke test: the shell, signals and SSE (64-bit kernel).

A system one can talk to rather than merely watch replay a script: a
program started FROM THE KEYBOARD, a signal stopping an endless loop
from the outside, a handler of one's own returning exactly where it was
interrupted, and a user shell joining programs with pipes.

At the end floating-point numbers. On x86-64 SSE is part of the calling
convention, so the compiler uses it without asking - and the XMM
registers are as much a task's state as the rest, and have to survive a
context switch.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
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

            # The keyboard and the shell: a system one can talk
            # to rather than merely watch replay a script
            ready = q23.wait_serial("thread64>", timeout=40)
            check("the 64-bit kernel starts the shell",
                  "PS/2 keyboard (IRQ 1)" in ready
                  and "The 64-bit system is ready" in ready)

            q23.type_line("mem")
            mem = q23.wait_serial("Memory:", timeout=20)
            check("the shell answers 'mem'",
                  re.search(r"Memory: \d+ KiB RAM, frames "
                            r"\d+/\d+ used", mem) is not None)
            q23.type_line("ls")
            lsout = q23.wait_serial("hello64  (", timeout=20)
            check("the shell lists the programs from the initrd",
                  "spawn64" in lsout and "sep64" in lsout)
            q23.type_line("ps")
            ps = q23.wait_serial("shell (current)", timeout=20)
            check("the shell sees itself on the task list",
                  "idle" in ps)

            # A program started FROM THE KEYBOARD, not from a script
            q23.type_line("run hello64")
            run = q23.wait_serial("done, exit code", timeout=40)
            check("a program started from the keyboard runs",
                  "[hello64] hello, I am a" in run)
            check("the shell waited out the program and gave its exit code",
                  "[hello64] done, exit code 7" in run)
            q23.type_line("nosuchcommand")
            bad = q23.wait_serial("Unknown command", timeout=20)
            check("the shell does not pretend to know every command",
                  "Unknown command: nosuchcommand" in bad)

            # Who is at the machine - the same two levels and the same
            # password as in protected mode, because it is the same
            # user.c on both sides
            q23.type_line("whoami")
            who = q23.wait_serial("user", timeout=20)
            check("the 64-bit kernel too comes up as an ordinary user",
                  "user (change the system" in who)
            q23.type_line("set color 13")
            no = q23.wait_serial("needs admin", timeout=20)
            check("changing a setting without admin is refused",
                  "needs admin" in no)
            q23.type_line("sudo set color 13")
            q23.wait_serial("password:", timeout=20)
            q23.type_line("thread")
            yes = q23.wait_serial("saved", timeout=30)
            check("once the password is given the setting is saved",
                  "color = 13 (saved)" in yes)
            q23.type_line("whoami")
            back = q23.wait_serial("user", timeout=20)
            check("and the right lasted one command",
                  "user (change the system" in back)

            # Signals: a program in an endless loop has to be
            # stoppable from the outside, because from the inside it
            # cannot be
            q23.type_line("run spin64")
            q23.wait_serial("[spin64] spinning", timeout=30)
            time.sleep(1)
            q23.sendkey("ctrl-c")
            kill = q23.wait_serial("[spin64] spinning", timeout=30)
            check("Ctrl+C sends a signal to the program",
                  re.search(r"task \d+ got signal 2", kill)
                  is not None)
            check("a program with no handler of its own dies of the signal",
                  "(spin64) killed: a signal" in kill)
            check("the shell takes the console back after the killed program",
                  "[spin64] done, exit code -1" in kill)

            # And a program with a handler of its own is to return
            # after it to where it was interrupted - that is the whole
            # difference between a signal and a kill
            q23.type_line("run sigtest64")
            q23.wait_serial("[sigtest64] waiting", timeout=30)
            time.sleep(1)
            q23.sendkey("ctrl-c")
            sig = q23.wait_serial("[sigtest64] I came back", timeout=30)
            check("the program catches the signal with a handler of its own",
                  "the handler: I got a signal" in sig)
            check("after the handler the program comes back where it was interrupted",
                  "I came back where I was interrupted" in sig)
            check("a caught signal does not kill the program",
                  "[sigtest64] done, exit code 0" in sig)
            check("the kernel does not report a death for a caught signal",
                  "(sigtest64) killed" not in sig)

            # Only what arrived AFTER this command - earlier in
            # the log both programs are there, of course
            seen = len(q23.serial())
            q23.type_line("ps")
            after = q23.wait_serial("shell (current)",
                                    timeout=20)[seen:]
            check("after both programs no task is left",
                  "spin64" not in after
                  and "sigtest64" not in after
                  and "idle" in after)

            # A shell in USER SPACE: an ordinary process with no
            # rights beyond those every other one has - it reads the
            # keyboard with a system call, and starts programs
            # through fork and exec
            q23.type_line("run ush64")
            u = q23.wait_serial("ush64$", timeout=40)
            check("the user's shell starts as a ring 3 process",
                  "ush64 - the user's shell" in u)

            # The word has to appear TWICE: once as the echo of
            # the typing, once as the program's answer. We count the
            # occurrences instead of looking for the end of a line -
            # the serial port ends them with the sequence "\n\r", and
            # tying oneself to such a detail is asking for trouble.
            q23.type_line("echo abrakadabra")
            limit = time.time() + 25
            while (time.time() < limit
                   and q23.serial().count("abrakadabra") < 2):
                time.sleep(0.3)
            ec = q23.serial()
            check("the program reads the keyboard with a system call"
                  f" ({ec.count('abrakadabra')} wystapien)",
                  ec.count("abrakadabra") >= 2)

            seen = len(q23.serial())
            q23.type_line("uecho64 from the user's shell")
            ua = q23.wait_serial("ush64: done, code 0",
                                 timeout=40)[seen:]
            check("the user's shell passes the arguments to exec",
                  "from the user's shell" in ua)

            # Pipes: one program's output enters another's
            # input, and neither of them knows about it. uupper64
            # reads from number zero and writes to number one - what
            # is on the other side is the business of whoever started
            # it.
            seen = len(q23.serial())
            q23.type_line("uecho64 thread through a pipe | uupper64")
            pp = q23.wait_serial("THREAD THROUGH A PIPE",
                                 timeout=40)[seen:]
            check("one program's output goes into another's input",
                  "THREAD THROUGH A PIPE" in pp)

            # The second program has to see the END of the data -
            # and that happens only when the shell closes both ends of
            # the pipe at its own end
            seen = len(q23.serial())
            q23.type_line("uls64 | uupper64")
            pl = q23.wait_serial("ULS64: ENTRIES", timeout=40)[seen:]
            check("the reader sees the end of the data and ends by itself",
                  "ULS64: ENTRIES" in pl)

            seen = len(q23.serial())
            q23.type_line("uecho64 alone |")
            pe = q23.wait_serial("there is nothing after the bar",
                                 timeout=30)[seen:]
            check("an empty side of the pipe does not upset the shell",
                  "ush64: there is nothing after the bar" in pe)

            q23.type_line("hello64")
            hi = q23.wait_serial("ush64: done, code 7", timeout=45)
            check("the user's shell starts a program (fork+exec)",
                  "[hello64] hello, I am a" in hi)
            check("and it takes its exit code through wait",
                  "ush64: done, code 7" in hi)

            # A failed exec must not kill the shell - the child
            # ends by itself, the parent lives on
            q23.type_line("nosuchprogram")
            ne = q23.wait_serial("ush64: done, code 127",
                                 timeout=30)
            check("a failed exec ends only the child",
                  "there is no program 'nosuchprogram'" in ne
                  and "ush64: done, code 127" in ne)

            # Ctrl+C in the user shell. The signal has to reach
            # the PROGRAM that is running, not the shell waiting for
            # it - and it has to reach a shell waiting inside the
            # kernel, because otherwise it waits there forever.
            q23.type_line("spin64")
            q23.wait_serial("[spin64] spinning", timeout=30)
            time.sleep(1.5)
            q23.sendkey("ctrl-c")
            ci = q23.wait_serial("ush64: done, code -1", timeout=30)
            check("Ctrl+C hits the program, not the waiting shell",
                  "(spin64) killed: a signal" in ci)
            check("the exit code of a killed program is negative",
                  "ush64: done, code -1" in ci)
            # And the shell itself is to survive it - otherwise
            # every accidental Ctrl+C would end the session
            seen = len(q23.serial())
            q23.type_line("echo poctrlc")
            limit = time.time() + 25
            while (time.time() < limit
                   and q23.serial()[seen:].count("poctrlc") < 2):
                time.sleep(0.3)
            check("the user's shell survived Ctrl+C and keeps reading",
                  q23.serial()[seen:].count("poctrlc") >= 2)

            q23.type_line("exit")
            ex = q23.wait_serial("[ush64] done, exit code 0",
                                 timeout=30)
            check("once the user's shell leaves the kernel's shell is back",
                  "ush64: pa!" in ex)

            # Floating-point numbers. On x86-64 SSE is part of
            # the calling convention, so the compiler uses it without
            # asking - and the XMM registers are as much a task's
            # state as the rest.
            sse = q23.wait_serial("The SSE state survived", timeout=40)
            check("the kernel switches the floating point unit on",
                  "The floating point unit and SSE are on"
                  in sse)
            results = re.findall(r"\[fpu64\] start (\d+): came out "
                                r"(\d+), wanted (\d+) - (\S+)", sse)
            check(f"two processes count at the same time "
                  f"({len(results)})", len(results) == 2)
            # Each is to get ITS OWN result - if the SSE state
            # were not saved, it would get the other one's
            check("each gets its own result, not somebody else's",
                  len(results) == 2
                  and results[0][1] != results[1][1]
                  and all(w[1] == w[2] for w in results))
            check("the kernel confirms there were no mistakes",
                  "2 counted, 0 mistakes" in sse)
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
