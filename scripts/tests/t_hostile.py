#!/usr/bin/env python3
"""ThreadOS smoke test: what a kernel audit said a program could do.

An outside review of the kernel (November 2026) listed ways an ordinary
ring 3 program could get past the kernel; user64/hostile64.c tries each
one that was real, and every refusal is held against the same call made
honestly, which has to go through:

  a background preview of 65536 x 65536 pixels into a small buffer -
  the size check multiplied in 32 bits, where that is zero, and the
  kernel would have painted 16 GiB into the program's memory

  a signal handler, or its return address, in kernel memory or in the
  program's data rather than its code

  an installation step - writing the disk - from a program nobody
  authorized, on a system started from a stick (only the installer the
  kernel itself starts on the whole screen may, there)

  programs written to the disk and started: one with a segment both
  writable and executable (W^X), one whose segment's file offset wraps
  round 2^64 to a small sum - the loader would have copied the kernel's
  memory before the file into the program - and one whose program
  header table does the same

  one account's program killing another account's at the same level -
  the call and the shell's 'kill' compared levels only; an admin still
  can

  guessing the admin password: the lock after three wrong ones grows
  (5 s, 10 s, ... a minute), so the right password six seconds into
  the second lock is refused, and taken once the lock ends

and SMEP: on a processor that has it (QEMU's 'max'), on every core, the
CR4 bit read by QEMU itself, with programs still running beside it; on
one that does not, the kernel says so and runs as before.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary


def fresh_disk():
    path = tempfile.mktemp(suffix=".img")
    with open(path, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    return path


VERDICTS = [
    "a 65536 x 65536 preview into a small buffer",
    "a signal handler in kernel memory",
    "a signal handler in the program's data",
    "a handler's return into kernel memory",
    "an installation step from an unauthorized program",
    "a segment both writable and executable",
    "a segment whose offset wraps round 2^64",
    "a header table whose offset wraps round 2^64",
]


def attempts(q):
    out = q.command("run hostile64", "[hostile64] done", timeout=120)
    check("the honest program off the disk runs (exit code 42) - the "
          "loader takes a well-made one",
          "the honest program ended with 42" in out)
    for what in VERDICTS:
        line = next((l for l in out.splitlines()
                     if l.startswith("[hostile64] " + what + ":")), "")
        check("%s: refused, and the honest call went through ('%s')"
              % (what, line[len("[hostile64] "):].strip()),
              ": refused (" in line)
    m = re.search(r"done: (\d+) refused, (\d+) holes, (\d+) unproven", out)
    check("nothing got through (%s)" % (m.group(0) if m else "no summary"),
          m is not None and m.group(2) == "0" and m.group(3) == "0"
          and int(m.group(1)) == len(VERDICTS))
    check("and the kernel is still standing",
          "thread64>" in q.command("whoami", "thread64>", timeout=30))


def other_accounts_program(q):
    # The guest leaves a program behind; signing in as somebody else
    # ends only a real account's processes (kernel/user.c), so it stays
    out = q.command("run hostile64 hold", "holding, pid", timeout=60)
    m = re.search(r"holding, pid (\d+)", out)
    check("the guest leaves a program running", m is not None)
    if not m:
        return
    pid = m.group(1)

    q.command("login admin", "password:", timeout=20)
    q.command("thread", "logged in as admin", timeout=30)
    q.command("useradd alice", "admin password:", timeout=20)
    q.command("thread", "new account password:", timeout=20)
    q.command("alicepass", "again:", timeout=20)
    out = q.command("alicepass", "account created", timeout=30)
    check("an account for alice", "account created" in out)
    q.command("login alice", "password:", timeout=20)
    q.command("alicepass", "logged in as alice", timeout=30)

    # alice, a plain user like the guest: the same level, another account
    out = q.command("run hostile64 kill " + pid, "[hostile64] kill", timeout=60)
    check("another account's program, at the same level, may not kill it "
          "(%s)" % next((l for l in out.splitlines() if "] kill" in l), ""),
          "[hostile64] kill %s: -2" % pid in out)
    out = q.command("kill " + pid, "kill:", timeout=20)
    check("nor may that account's shell", "is not yours to end" in out)

    q.command("login admin", "password:", timeout=20)
    q.command("thread", "logged in as admin", timeout=30)
    out = q.command("kill " + pid, "kill:", timeout=20)
    check("an admin may - so it was there to be killed",
          "kill: %s ended" % pid in out)


def guessing(q):
    out = q.command("run hostile64 guess thread", "11 s into it", timeout=90)
    m = re.search(r"6 s into the second lock: (-?\d+), 11 s into it: (-?\d+)",
                  out)
    check("the right password is refused six seconds into the second lock "
          "- it grew to ten (%s)" % (m.group(1) if m else "-"),
          m is not None and m.group(1) == "-2")
    check("and taken once it is over (%s)" % (m.group(2) if m else "-"),
          m is not None and m.group(2) == "0")


def smep():
    disk = fresh_disk()
    log = tempfile.mktemp(suffix=".log")
    try:
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=log,
                 sata=True, cpu_model="max", cpus=2)
        try:
            out = q.wait_serial("thread64>", timeout=120)
            check("on a processor with SMEP the kernel turns it on",
                  "[ OK ] SMEP: the kernel will not run a program's pages"
                  in out)
            q.command("run hello64", "exit code", timeout=60)
            regs = q.hmp("info registers -a")["return"]
            cr4 = [int(v, 16) for v in re.findall(r"CR4=([0-9a-f]+)", regs)]
            check("on every core, as QEMU reads CR4 (%s)"
                  % ", ".join("%x" % v for v in cr4),
                  len(cr4) == 2 and all(v & (1 << 20) for v in cr4))
            out = q.command("run hostile64", "[hostile64] done", timeout=120)
            check("and programs run beside it, the attempts refused as "
                  "before", "0 holes, 0 unproven" in out)
        finally:
            q.close()
    finally:
        for f in (disk, log):
            if os.path.exists(f):
                os.unlink(f)


def main():
    print("--- what an audit said a program could do ---")
    disk = fresh_disk()
    log = tempfile.mktemp(suffix=".log")
    try:
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=log,
                 sata=True)
        try:
            out = q.wait_serial("thread64>", timeout=90)
            check("without SMEP in the processor the kernel says so and "
                  "carries on", "[ -- ] SMEP: this processor does not "
                  "have it" in out)
            attempts(q)
            other_accounts_program(q)
            guessing(q)
        finally:
            q.close()
    finally:
        for f in (disk, log):
            if os.path.exists(f):
                os.unlink(f)
    smep()
    return summary()


if __name__ == "__main__":
    sys.exit(main())
