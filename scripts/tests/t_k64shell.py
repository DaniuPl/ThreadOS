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


def answer(q, cmd, until, timeout):
    """Types a command and reads what arrived after it until until(text)
    says the answer is all there, or the time runs out."""
    start = len(q.serial())
    q.type_line(cmd)
    deadline = time.time() + timeout
    out = ""
    while time.time() < deadline:
        out = q.serial()[start:]
        if until(out):
            break
        time.sleep(0.3)
    return out


def fresh_after(q, seen, want, timeout):
    """What arrived after position 'seen', once 'want' - a text or a
    compiled pattern - is among it (or the time ran out)."""
    found = want.search if hasattr(want, "search") else (lambda t: want in t)
    deadline = time.time() + timeout
    while time.time() < deadline and not found(q.serial()[seen:]):
        time.sleep(0.3)
    return q.serial()[seen:]


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
            # The message says what was found rather than that something
            # was: a machine with nothing on the auxiliary port says so
            # instead of reporting a mouse it has not got
            check("the PS/2 mouse was found at boot and its wheel counted",
                  re.search(r"PS/2 keyboard \(IRQ 1\) and mouse \(IRQ 12\), "
                            r"wheel: (yes|no)", ready) is not None)

            # Only what came after each command counts - the boot wrote
            # about memory, programs and tasks too - and only once the
            # answer is there whole. 'Swap:' is the last line 'mem'
            # prints; 'ps' ends where the prompt comes back.
            mem = answer(q23, "mem", lambda o: "Swap:" in o, timeout=20)
            check("the shell answers 'mem'",
                  re.search(r"Memory: \d+ KiB RAM, frames "
                            r"\d+/\d+ used", mem) is not None)
            # All the detected memory is under management, not only the
            # first 64 MiB: the identity mapping stretches all the way to
            # the top of the RAM.
            ram = re.search(r"Memory: (\d+) KiB RAM", mem)
            ident = re.search(r"Paging: identity-mapped (\d+) MiB", mem)
            check("the identity mapping reaches the top of the RAM found"
                  + (f" ({ident.group(1)} MiB of "
                     f"{int(ram.group(1)) // 1024} MiB)"
                     if ram and ident else ""),
                  ram is not None and ident is not None
                  and int(ident.group(1)) >= int(ram.group(1)) // 1024 - 2)
            listed = ("  hello64  (", "  spawn64  (", "  sep64  (")
            lsout = answer(q23, "ls",
                           lambda o: all(name in o for name in listed),
                           timeout=20)
            check("the shell lists the programs from the initrd",
                  all(name in lsout for name in listed))
            ps_whole = re.compile(r"ID +STATE +NAME[\s\S]*?thread64>")
            ps = answer(q23, "ps", ps_whole.search, timeout=20)
            check("the shell sees itself on the task list",
                  "shell (current)" in ps and "idle" in ps)

            # Every task's parent, as a program sees it through the
            # system call: 'core ps' is the ps the terminal runs, and the
            # shell that started it has to be named as its parent
            cps = q23.command("run core ps", "[core] done, exit code",
                              timeout=40).replace("\r", "")
            rows = re.findall(r"^ *(\d+) +(\d+) +(\w+) +(\S+)( \[kernel\])?$",
                              cps, re.M)
            shells = {pid for pid, _, _, name, kern in rows
                      if name == "shell" and kern}
            core = [parent for _, parent, _, name, kern in rows
                    if name == "core" and not kern]
            check("'core ps' shows every task's parent"
                  + (f" (core's parent {core[0]}, shells {sorted(shells)})"
                     if core else ""),
                  "PID  PARENT  STATE" in cps and len(core) == 1
                  and core[0] in shells)

            # A process with threads and memory from the mapping area gives
            # everything back when its last thread ends - the threads'
            # stacks and the pages they asked for. The shell counts the
            # frames before the start and after the end.
            # The wait is for the shell's whole report of the end, frame
            # counts included - its first words alone can be on the port
            # while the numbers checked are still on the way.
            def bill(name):
                return re.compile(r"\[%s\] done, exit code (-?\d+) "
                                  r"\(frames (\d+) -> (\d+)" % name)

            tl = answer(q23, "run tlbtest64", bill("tlbtest64").search,
                        timeout=180)
            m = bill("tlbtest64").search(tl)
            check("a process with threads and mapped memory leaks no frames"
                  + (f" ({m.group(2)} -> {m.group(3)})" if m else ""),
                  m is not None and m.group(1) == "0"
                  and int(m.group(3)) <= int(m.group(2)))

            # A program started FROM THE KEYBOARD, not from a script.
            # Only what came after the command counts, and only once the
            # shell's line has arrived whole - an earlier program's
            # 'done, exit code' is in the log already.
            ended = re.compile(r"\[hello64\] done, exit code -?\d+ \(")
            run = answer(q23, "run hello64", ended.search, timeout=40)
            check("a program started from the keyboard runs",
                  "[hello64] hello, I am a" in run)
            check("the shell waited out the program and gave its exit code",
                  "[hello64] done, exit code 7 (" in run)
            q23.type_line("nosuchcommand")
            bad = q23.wait_serial("Unknown command", timeout=20)
            check("the shell does not pretend to know every command",
                  "Unknown command: nosuchcommand" in bad)

            # The keyboard: the echo comes letter by letter as the keys
            # arrive, and a capital comes from the shift key. The keys
            # come from the string itself - a hand-written list of
            # letters is a trap for whoever renames the word.
            seen = len(q23.serial())
            typed = "thread 1"
            for ch in typed:
                q23.sendkey("spc" if ch == " " else ch)
            q23.sendkey("shift-x")
            limit = time.time() + 20
            while (time.time() < limit
                   and typed + "X" not in q23.serial()[seen:]):
                time.sleep(0.3)
            echo = q23.serial()[seen:]
            check(f"the keyboard echo: '{typed}'", typed in echo)
            check("the keyboard echo with shift: 'X'", typed + "X" in echo)
            q23.sendkey("ret")
            bad = q23.wait_serial("Unknown command: " + typed + "X",
                                  timeout=20)[seen:]
            check("the line typed with shift reaches the shell whole",
                  "Unknown command: %sX (try 'help')" % typed in bad)

            # Who is at the machine - two levels and a password, kept by
            # the shared user.c
            # 'whoami' answers with the name and the level in brackets;
            # the word 'user' alone is in the log long before it is asked
            level = re.compile(r"\((user|admin)\)")
            who = answer(q23, "whoami", level.search, timeout=20)
            check("the 64-bit kernel too comes up as an ordinary user",
                  "guest (user)" in who)
            q23.type_line("set color 13")
            no = q23.wait_serial("needs admin", timeout=20)
            check("changing a setting without admin is refused",
                  "needs admin" in no)
            q23.type_line("sudo set color 13")
            q23.wait_serial("password:", timeout=20)
            seen = len(q23.serial())
            q23.type_line("thread")
            yes = fresh_after(q23, seen, "(saved)", timeout=30)
            check("once the password is given the setting is saved",
                  "color = 13 (saved)" in yes)
            back = answer(q23, "whoami", level.search, timeout=20)
            check("and the right lasted one command",
                  "guest (user)" in back)

            # Signals: a program in an endless loop has to be
            # stoppable from the outside, because from the inside it
            # cannot be
            seen = len(q23.serial())
            q23.type_line("run spin64")
            fresh_after(q23, seen, "[spin64] tick 2", timeout=30)
            q23.sendkey("ctrl-c")
            kill = fresh_after(q23, seen, bill("spin64"), timeout=30)
            check("Ctrl+C sends a signal to the program",
                  re.search(r"task \d+ got signal 2", kill)
                  is not None)
            check("a program with no handler of its own dies of the signal",
                  "(spin64) killed: a signal" in kill)
            check("the shell takes the console back after the killed program",
                  "[spin64] done, exit code -1 (" in kill)

            # And a program with a handler of its own is to return
            # after it to where it was interrupted - that is the whole
            # difference between a signal and a kill. sigtest64 sets its
            # handler before it says it waits, so the key can follow the
            # line at once.
            seen = len(q23.serial())
            q23.type_line("run sigtest64")
            fresh_after(q23, seen, "[sigtest64] waiting", timeout=30)
            q23.sendkey("ctrl-c")
            sig = fresh_after(q23, seen, bill("sigtest64"), timeout=30)
            check("the program catches the signal with a handler of its own",
                  "the handler: I got a signal" in sig)
            check("after the handler the program comes back where it was interrupted",
                  "I came back where I was interrupted" in sig)
            check("a caught signal does not kill the program",
                  "[sigtest64] done, exit code 0 (" in sig)
            check("the kernel does not report a death for a caught signal",
                  "(sigtest64) killed" not in sig)
            # Both programs give back every frame they had - the one that
            # died of the signal as much as the one that caught it. The
            # shell counts the frames before the start and after the end.
            bills = [bill(name).search(log)
                     for name, log in (("spin64", kill), ("sigtest64", sig))]
            check("no frames leaked after the signals ("
                  + ", ".join(f"{b.group(2)} -> {b.group(3)}" if b else "?"
                              for b in bills) + ")",
                  all(b is not None and int(b.group(3)) <= int(b.group(2))
                      for b in bills))

            # A handler's frame goes BELOW everything on the program's
            # stack, so it can land in a page the stack has not grown
            # into yet. spin64 'edge' stands 64 bytes above the bottom of
            # the lowest page it touched; the kernel has to grow the
            # stack for the frame, as the program's own push would, and
            # not call it full.
            # spin64 writes the line waited for only once its stack
            # pointer is at the edge, so no pause is needed before the
            # key; and the handler prints its success line only when its
            # own frame is in the page below the edge, not merely when it
            # ran.
            seen = len(q23.serial())
            q23.type_line("run spin64 edge")
            fresh_after(q23, seen, "[spin64] waiting at the edge of the "
                        "stack - stop me with Ctrl+C", timeout=30)
            q23.sendkey("ctrl-c")
            edge = fresh_after(q23, seen,
                               re.compile(r"\[spin64\] done, exit code "
                                          r"-?\d+ \("), timeout=30)
            check("a handler runs even when its frame lands below the "
                  "stack touched so far",
                  "[spin64] the handler ran below the edge of the stack"
                  in edge
                  and "no room on the program's stack" not in edge
                  and "not in the page below the edge" not in edge)
            check("and the program ends by itself from it",
                  re.search(r"\[spin64\] done, exit code 0 \(", edge)
                  is not None)

            # A program killed is owed what one that exits gets: what it
            # wrote into a writable mapping goes back to the file. spin64
            # writes its mark through a mapping, never flushes it, and
            # dies of Ctrl+C - the mark has to be in the file afterwards.
            made = q23.command("write spinmark 123456789012345-and-the-rest",
                               "written", timeout=20)
            check("the file for the killed program is written",
                  "'spinmark' written" in made)
            seen = len(q23.serial())
            q23.type_line("run spin64 spinmark")
            marked = fresh_after(q23, seen, "[spin64] tick 2", timeout=30)
            check("spin64 wrote its mark through the mapping",
                  "[spin64] marked 'spinmark' and will not flush it"
                  in marked)
            q23.sendkey("ctrl-c")
            died = fresh_after(q23, seen, "[spin64] done, exit code",
                               timeout=30)
            check("and it was killed, not ended",
                  "(spin64) killed: a signal" in died)
            kept = q23.command("cat spinmark", "-and-the-rest", timeout=20)
            check("what a killed program wrote into a mapping reached the "
                  "file",
                  "killed-but-kept-and-the-rest" in kept)

            # Only what arrived AFTER this command - earlier in
            # the log both programs are there, of course - and the list
            # whole, up to the prompt: the shell is its first row, so the
            # rows that matter come after it
            after = answer(q23, "ps", ps_whole.search, timeout=20)
            check("after both programs no task is left",
                  "spin64" not in after
                  and "sigtest64" not in after
                  and "shell (current)" in after
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
            q23.type_line("echo abracadabra")
            limit = time.time() + 25
            while (time.time() < limit
                   and q23.serial().count("abracadabra") < 2):
                time.sleep(0.3)
            ec = q23.serial()
            check("the program reads the keyboard with a system call"
                  f" ({ec.count('abracadabra')} occurrences)",
                  ec.count("abracadabra") >= 2)

            # Twice here as well: the echo of the typing has the words
            # whether or not the program ever got them. And the shell's
            # word about the end counts only once it is there whole,
            # line end and all.
            ush_done = re.compile(r"ush64: done, code -?\d+\s")
            seen = len(q23.serial())
            q23.type_line("uecho64 from the user's shell")
            ua = fresh_after(q23, seen, ush_done, timeout=40)
            said = ua.count("from the user's shell")
            check("the user's shell passes the arguments to exec"
                  f" ({said} occurrences)",
                  said >= 2 and "ush64: done, code 0" in ua)

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

            # hello64 ran from the kernel's shell earlier, and spin64
            # was killed there: only what came after each command counts
            # - and only once the code is there whole, line end and all
            seen = len(q23.serial())
            q23.type_line("hello64")
            hi = fresh_after(q23, seen, ush_done, timeout=45)
            check("the user's shell starts a program (fork+exec)",
                  "[hello64] hello, I am a" in hi)
            check("and it takes its exit code through wait",
                  "ush64: done, code 7" in hi)

            # A failed exec must not kill the shell - the child
            # ends by itself, the parent lives on
            seen = len(q23.serial())
            q23.type_line("nosuchprogram")
            ne = fresh_after(q23, seen, ush_done, timeout=30)
            check("a failed exec ends only the child",
                  "there is no program 'nosuchprogram'" in ne
                  and "ush64: done, code 127" in ne)

            # Ctrl+C in the user shell. The signal has to reach
            # the PROGRAM that is running, not the shell waiting for
            # it - and it has to reach a shell waiting inside the
            # kernel, because otherwise it waits there forever.
            seen = len(q23.serial())
            q23.type_line("spin64")
            fresh_after(q23, seen, "[spin64] tick 2", timeout=30)
            q23.sendkey("ctrl-c")
            ci = fresh_after(q23, seen, ush_done, timeout=30)
            check("Ctrl+C hits the program, not the waiting shell",
                  "(spin64) killed: a signal" in ci)
            check("the exit code of a killed program is negative",
                  "ush64: done, code -1" in ci)
            # And the shell itself is to survive it - otherwise
            # every accidental Ctrl+C would end the session
            seen = len(q23.serial())
            q23.type_line("echo afterctrlc")
            limit = time.time() + 25
            while (time.time() < limit
                   and q23.serial()[seen:].count("afterctrlc") < 2):
                time.sleep(0.3)
            check("the user's shell survived Ctrl+C and keeps reading",
                  q23.serial()[seen:].count("afterctrlc") >= 2)

            # Ctrl+C with nothing running reaches the shell itself, which
            # is waiting for a key inside the kernel. The wait is
            # interrupted halfway: the line begun is dropped, never run,
            # and the shell carries on.
            seen = len(q23.serial())
            q23.type_line("echo halfway", enter=False)
            limit = time.time() + 20
            while (time.time() < limit
                   and "halfway" not in q23.serial()[seen:]):
                time.sleep(0.3)
            q23.sendkey("ctrl-c")
            limit = time.time() + 20
            while (time.time() < limit
                   and "^C" not in q23.serial()[seen:]):
                time.sleep(0.3)
            check("Ctrl+C reaches the reading shell as SIGINT and interrupts the read",
                  "^C" in q23.serial()[seen:])
            q23.type_line("echo afterwards")
            limit = time.time() + 25
            while (time.time() < limit
                   and q23.serial()[seen:].count("afterwards") < 2):
                time.sleep(0.3)
            after = q23.serial()[seen:]
            check("the interrupted line was dropped and the shell reads on",
                  after.count("afterwards") >= 2
                  and after.count("halfway") == 1)

            # A program killed by a CPU exception ends with a code of its
            # own too. Only what came after the command counts: wild64
            # already died once at boot, and so did spin64 above.
            seen = len(q23.serial())
            q23.type_line("wild64")
            wi = fresh_after(q23, seen, ush_done, timeout=40)
            check("the exit code of a process killed by an exception (-1)",
                  "(wild64) killed: Page Fault" in wi
                  and "ush64: done, code -1" in wi)

            # A program killed by the processor rather than by a signal:
            # its end has to reach the shell's wait all the same, as a
            # negative code
            seen = len(q23.serial())
            q23.type_line("nx64")
            limit = time.time() + 30
            while (time.time() < limit
                   and "ush64: done, code" not in q23.serial()[seen:]):
                time.sleep(0.3)
            nx = q23.serial()[seen:]
            check("the code of a program killed by an exception reaches "
                  "the shell (-1)",
                  "(nx64) killed: Page Fault" in nx
                  and "ush64: done, code -1" in nx)

            # A pipe carries exactly what was written: 'core wc' counts
            # the bytes that came through - "thread-pipe" and a newline
            seen = len(q23.serial())
            q23.type_line("uecho64 thread-pipe | core wc")
            limit = time.time() + 30
            while (time.time() < limit and re.search(
                    r"^\s*\d+\s+\d+\s+\d+\s*$",
                    q23.serial()[seen:].replace("\r", ""), re.M) is None):
                time.sleep(0.3)
            wc = re.findall(r"^\s*(\d+)\s+(\d+)\s+(\d+)\s*$",
                            q23.serial()[seen:].replace("\r", ""), re.M)
            check("a pipe into 'core wc': 1 line, 1 word, 12 bytes (%s)"
                  % (" ".join(wc[-1]) if wc else "-"),
                  bool(wc) and wc[-1] == ("1", "1", "12"))

            q23.type_line("exit")
            ex = q23.wait_serial("[ush64] done, exit code 0",
                                 timeout=30)
            check("once the user's shell leaves the kernel's shell is back",
                  "ush64: bye!" in ex)

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
