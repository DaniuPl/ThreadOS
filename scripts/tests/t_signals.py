#!/usr/bin/env python3
"""ThreadOS smoke test: signals.

There is one sharpest point here: the return from a handler. The signal
arrives while the program is somewhere in its own loop or inside a
system call, and the handler runs in ring 3 on the program's stack - and
has to return in such a way that the program notices nothing.

Every signal but one goes through the keyboard: Ctrl+C reaches the
process holding the console as a SIGINT. The others are sent by a
program, 'core kill', typed on the second console while the first is
busy - the way one ends a program that holds its own console. SIGTERM
ends a program with no handler and is caught by one that has a handler;
SIGKILL is caught by nobody.

Everything is read from the serial port, which gets every console's
text: GRUB gives the kernel a framebuffer, so the VGA character memory
says nothing.

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


def until(q, start, text, timeout=30):
    """What came on the port since 'start', once 'text' is in it"""
    end = time.time() + timeout
    while time.time() < end:
        fresh = q.serial()[start:]
        if text in fresh:
            return fresh
        time.sleep(0.3)
    return q.serial()[start:]


def no_leak(said, name):
    """The kernel shell's line after a program: frames before -> after"""
    m = re.search(r"\[%s\] done, exit code -?\d+ \(frames (\d+) -> (\d+)"
                  % name, said)
    return m is not None and int(m.group(2)) <= int(m.group(1)), \
        (m.group(1) + " -> " + m.group(2)) if m else "no line"


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    slog = tempfile.mktemp(suffix=".log")

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
    try:
        q.wait_serial("thread64>", timeout=90)
        q.wait_serial("[net] DHCP", timeout=30)
        time.sleep(0.5)

        # --- A handler of one's own, and Ctrl+C from a real keyboard ---
        seen = len(q.serial())
        q.type_line("run sigtest64")
        until(q, seen, "[sigtest64] waiting", timeout=30)
        time.sleep(1)
        q.sendkey("ctrl-c")
        out = until(q, seen, "[sigtest64] done, exit code")
        check("Ctrl+C reaches the program as a signal, and its own handler "
              "runs in ring 3", "[sigtest64] the handler: I got a signal"
              in out)
        check("after the handler the program comes back where it was "
              "interrupted, and ends by itself (code 0)",
              "I came back where I was interrupted" in out
              and "[sigtest64] done, exit code 0" in out)
        ok, said = no_leak(out, "sigtest64")
        check(f"no frames leaked after the signal ({said})", ok)

        # --- Signals sent by another program ---------------------------
        # The first console's shell waits for its program; the second
        # console has a shell of its own, and from there 'core kill'
        # finds the program by name.
        def kill_from_second(target, args, wait):
            """Starts 'target' on the first console, kills it from the
            second, comes back. What came on the port meanwhile."""
            q.sendkey("alt-f1")
            time.sleep(0.5)
            start = len(q.serial())
            q.type_line("run " + target)
            until(q, start, "[%s] " % target, timeout=30)
            time.sleep(1)
            q.sendkey("alt-f2")
            time.sleep(0.5)
            q.type_line("run core kill " + args)
            out = until(q, start, wait)
            out = until(q, start, "[core] done, exit code")
            q.sendkey("alt-f1")
            time.sleep(0.5)
            return out

        # SIGTERM to a program with no handler: the default is death
        out = kill_from_second("spin64", "spin64", "[spin64] done")
        check("'core kill' by name sends SIGTERM to the program",
              re.search(r"task \d+ got signal 15", out) is not None
              and "[core] done, exit code 0" in out)
        check("SIGTERM ends a program with no handler of its own",
              "(spin64) killed: a signal" in out
              and "[spin64] done, exit code -1" in out)
        ok, said = no_leak(out, "spin64")
        check(f"no frames leaked after the killed program ({said})", ok)

        # SIGTERM to a program WITH a handler: it is caught like Ctrl+C,
        # and the program carries on to its own end
        out = kill_from_second("sigtest64", "sigtest64",
                               "[sigtest64] done")
        check("a handler catches SIGTERM too, and the program ends by "
              "itself (code 0)",
              "[sigtest64] the handler: I got a signal" in out
              and "[sigtest64] done, exit code 0" in out
              and "(sigtest64) killed" not in out)

        # SIGKILL to the same program: the handler is there, and is
        # never called
        out = kill_from_second("sigtest64", "sigtest64 9",
                               "[sigtest64] done")
        check("SIGKILL cannot be caught: the program with a handler dies",
              re.search(r"task \d+ got signal 9", out) is not None
              and "(sigtest64) killed: a signal" in out
              and "[sigtest64] done, exit code -1" in out)
        check("and its handler never ran",
              "the handler: I got a signal" not in out)

        # A name nobody has: the program says so, and fails
        start = len(q.serial())
        q.sendkey("alt-f2")
        time.sleep(0.5)
        q.type_line("run core kill nosuchprogram")
        out = until(q, start, "[core] done, exit code")
        check("'kill' of a name nobody has is refused in words",
              "kill: no such process 'nosuchprogram'" in out
              and "[core] done, exit code 1" in out)
        q.sendkey("alt-f1")
        time.sleep(0.5)

        # --- Ctrl+C in the middle of a read --------------------------
        # ush64 waits for a key inside sys_read. Ctrl+C has to break
        # that wait halfway: the handler runs, the read comes back with
        # nothing, and the half-typed line is thrown away.
        start = len(q.serial())
        q.type_line("run ush64")
        until(q, start, "ush64$", timeout=30)
        q.type_line("halfway", enter=False)
        time.sleep(0.5)
        start = len(q.serial())
        q.sendkey("ctrl-c")
        out = until(q, start, "ush64$", timeout=20)
        check("Ctrl+C breaks a read halfway, and the shell is back at its "
              "prompt", "^C" in out and "ush64$" in out)
        start = len(q.serial())
        q.type_line("echo afterwards")
        out = until(q, start, "ush64$", timeout=20)
        check("and the half-typed line was thrown away, not kept",
              re.search(r"^afterwards\s*$", out.replace("\r", ""), re.M)
              is not None and "halfway" not in out)
        start = len(q.serial())
        q.type_line("exit")
        out = until(q, start, "[ush64] done, exit code")
        check("the interrupted shell leaves cleanly (code 0)",
              "[ush64] done, exit code 0" in out)
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
