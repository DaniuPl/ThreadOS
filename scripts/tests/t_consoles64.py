#!/usr/bin/env python3
"""ThreadOS smoke test: virtual consoles in the 64-bit kernel.

The hardware has one screen, and the system holds four. Alt+F1..F4
switches the picture, but every console has its own content, its own
shell and its own writers: which console a program writes on is decided
by the TASK, not by what we happen to be looking at. That is why a
program started on the second console keeps writing on it while we look
at the first - and that is this test's real assertion, stronger than the
picture switching itself.

The whole console buffer is in kernel/vga.c; task ownership (task_vc)
and a separate key queue per console come from the kernel's task and
keyboard code. The second half sends signals from one console to a
program on another with 'core kill': the program's own output, and the
kernel's word about its death, land on its console and nowhere else -
and SIGKILL ends it even though it has a handler of its own.

We read the screen from the framebuffer rather than from the character
memory at 0xB8000: GRUB, seeing a request for a video mode in the
kernel's header, gives graphics anyway, so the console draws itself in
an 8x8 font. So the reading is the one a human gets - from what can
really be seen.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64

# spin64 says this every half second, with a number that grows
TICK = re.compile(r"\[spin64\] tick (\d+)")


def ticks(text):
    return [int(n) for n in TICK.findall(text)]


def main():
    print("--- virtual consoles Alt+F1..F4 (the 64-bit kernel) ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    try:
        check("the disc with the 64-bit kernel builds", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=60)

            # The first console: something that will stay on it for a while
            q.type_line("clear")
            q.type_line("uptime")
            time.sleep(1.0)
            first_one = q.wait_fbcon("Uptime:", timeout=10)
            check("the first console works as before",
                  "Uptime:" in first_one)

            # Alt+F2: the second console has its own shell and is empty
            q.sendkey("alt-f2")
            time.sleep(0.8)
            second_one = q.screen_fbcon()
            check("Alt+F2 shows the second console with a prompt of its own",
                  "console 2" in second_one)
            check("what is on the first console does not leak onto the second",
                  "Uptime:" not in second_one)

            # A program started on the second console writes on it
            q.type_line("run spin64")
            view = q.wait_fbcon("[spin64] tick", timeout=20)
            check("a program started on the second console writes to it",
                  "[spin64] tick" in view)

            # Back to the first: its content untouched, and the program
            # from the second still writes on its own, not here - it
            # ticks every half second, so a tick landing on the wrong
            # console would be on this screen by now
            q.sendkey("alt-f1")
            time.sleep(1.5)
            returned = q.screen_fbcon()
            check("the first console is untouched when we come back",
                  "Uptime:" in returned)
            check("a program on the second console does not write over the first",
                  "[spin64]" not in returned)

            # Ctrl+C concerns the console it falls on: here there is
            # nothing to interrupt, and the program on the second is to
            # keep running. Keys reach a console in order, so once the
            # answer to the command typed after Ctrl+C is there, Ctrl+C
            # has been dealt with - and the ticks have to go on after it.
            before_ctrlc = len(q.serial())
            q.sendkey("ctrl-c")
            q.command("uptime", "Uptime:", timeout=20)
            seen = len(q.serial())
            end = time.time() + 15
            while time.time() < end and len(ticks(q.serial()[seen:])) < 2:
                time.sleep(0.3)
            fresh = ticks(q.serial()[seen:])
            check("a program on the second console survived Ctrl+C on the "
                  "first (%d ticks after it)" % len(fresh),
                  len(fresh) >= 2
                  and "(spin64) killed" not in q.serial()[before_ctrlc:])
            q.sendkey("alt-f2")
            end = time.time() + 15
            shown = []
            while time.time() < end and fresh:
                shown = ticks(q.screen_fbcon())
                if shown and max(shown) >= fresh[-1]:
                    break
                time.sleep(0.4)
            check("and its ticks went on on its own console (%s on the "
                  "screen, %s on the port)"
                  % (max(shown) if shown else "none",
                     fresh[-1] if fresh else "none"),
                  bool(shown) and bool(fresh) and max(shown) >= fresh[-1])

            # Ctrl+C on its own console ends the program. The proof is
            # double: the shell reports the child's end, and after the
            # shell's prompt nothing more comes - neither on the port nor
            # on the screen. Alive, spin64 would tick four times in the
            # two seconds we watch.
            seen = len(q.serial())
            q.sendkey("ctrl-c")
            end = time.time() + 30
            ended = re.compile(r"\[spin64\] done, exit code (-?\d+) "
                               r"\([^\n]*\n[\s\S]*thread64> ")
            while time.time() < end and not ended.search(q.serial()[seen:]):
                time.sleep(0.3)
            m = ended.search(q.serial()[seen:])
            check("Ctrl+C on its own console interrupts the program",
                  m is not None and m.group(1) == "-1")
            # The screen is the baseline once the prompt is drawn too:
            # two readings half a second apart that agree
            after = q.wait_fbcon("[spin64] done, exit code", timeout=10)
            for _ in range(10):
                time.sleep(0.5)
                now = q.screen_fbcon()
                if now == after:
                    break
                after = now
            quiet_from = len(q.serial())
            time.sleep(2.0)
            check("the interrupted program stopped writing",
                  not ticks(q.serial()[quiet_from:])
                  and q.screen_fbcon() == after)

            # A program writes on ITS console even while another one is
            # shown. sigtest64 waits on the second console with a handler
            # of its own, and the signal comes from the first, through
            # 'core kill' - so whatever the handler prints, it prints
            # while the second console is out of sight. We wait on the
            # serial port, which gets every console, until both shells
            # have reported their child's end, and only then look at the
            # screens.
            def both_ended(seen, timeout=40):
                end = time.time() + timeout
                while (time.time() < end
                       and q.serial()[seen:].count("] done, exit code") < 2):
                    time.sleep(0.3)
                return q.serial()[seen:]

            q.type_line("clear")
            q.type_line("run sigtest64")
            q.wait_fbcon("[sigtest64] waiting", timeout=30)
            q.sendkey("alt-f1")
            time.sleep(1.0)
            q.type_line("clear")
            seen = len(q.serial())
            q.type_line("run core kill sigtest64 2")
            both_ended(seen)
            first = q.wait_fbcon("[core] done, exit code", timeout=20)
            check("'core kill' on the first console reaches the program on the second",
                  "[core] done, exit code 0" in first)
            check("what that program printed did not land on the first console",
                  "[sigtest64]" not in first)
            q.sendkey("alt-f2")
            second = q.wait_fbcon("[sigtest64] I came back", timeout=20)
            check("the program wrote on its own console while another was shown",
                  "the handler: I got a signal" in second
                  and "[sigtest64] I came back" in second)
            # Each console's shell waits for its own child and has to name
            # that child when it ends - not whatever the other console's
            # shell started in the meantime
            check("the second console's shell names its own program when it "
                  "ends (code 0)",
                  "[sigtest64] done, exit code 0" in second)

            # SIGKILL is the one signal a handler cannot catch: the same
            # program with the same handler, and the kernel ends it anyway
            # - from the first console, while the second is not shown
            q.type_line("clear")
            q.type_line("run sigtest64")
            q.wait_fbcon("[sigtest64] waiting", timeout=30)
            q.sendkey("alt-f1")
            time.sleep(1.0)
            q.type_line("clear")
            seen = len(q.serial())
            q.type_line("run core kill sigtest64 9")
            killed = both_ended(seen)
            check("SIGKILL ends a program that has a handler of its own",
                  re.search(r"task \d+ got signal 9", killed) is not None
                  and "(sigtest64) killed: a signal" in killed)
            check("the handler never ran for SIGKILL",
                  "the handler: I got a signal" not in killed)
            # The kernel says it on the dying program's console: the
            # first one, from which the kill came, gets only core's own
            # end
            first = q.wait_fbcon("[core] done, exit code", timeout=20)
            check("the kernel's word about the kill did not land on the "
                  "first console",
                  "[core] done, exit code 0" in first
                  and "(sigtest64) killed" not in first
                  and "got signal 9" not in first)
            q.sendkey("alt-f2")
            second = q.wait_fbcon("(sigtest64) killed", timeout=20)
            check("kill from the first console ends the second console's "
                  "program, and the kernel says so there",
                  "(sigtest64) killed: a signal" in second)
            check("the second console's shell names its own program when it "
                  "ends (code -1)",
                  "[sigtest64] done, exit code -1" in second)
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
