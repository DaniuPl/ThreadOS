#!/usr/bin/env python3
"""ThreadOS smoke test: who is at the machine, and what that lets them do.

Everything that ran could do everything. A program in ring 3 could
overwrite the file the system reads its settings from at boot, delete
the swap file, or change the machine's name - and so could anybody who
typed a command. There was nobody to ask, because there was nobody.

There are two now: an admin, who may change the system, and a plain
user, who may do everything else. Two levels, no groups, no owners per
file - enough to say the thing worth saying, that changing the system
is a different act from using it.

What is checked here is the whole road, not the flag:

  the refusal is REAL - after it, the setting still has its old value,
  and the system file is still there
  the password is the only way up, a wrong one does not open anything,
  and three wrong ones in a row shut the door for a while
  the right lasts for ONE command: after 'sudo set ...' the next
  command is a plain user's again
  a program in ring 3 is refused by the KERNEL, not by the program: rm
  and cp are told 'not allowed', which is a different answer from
  'there is no such file'
  the new password survives a restart, and the file on the disk holds
  a number computed from it rather than the password itself

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

DISK_MB = 16


def new_disk():
    path = tempfile.mktemp(suffix=".img")
    with open(path, "wb") as f:
        f.truncate(DISK_MB * 1024 * 1024)
    return path


def sudo(q, command, password, wait, timeout=30):
    """'sudo <command>', then the password on the line the shell asks
    for it on. The password never appears on the screen, so we wait for
    what comes after it."""
    q.type_line("sudo " + command)
    time.sleep(1)
    return q.command(password, wait, timeout=timeout)


def main():
    disk = new_disk()
    slog = tempfile.mktemp(suffix=".log")

    print("--- an ordinary user ---")
    try:
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
        try:
            q.wait_for("ThreadOS shell", timeout=40)

            out = q.command("whoami", "user", timeout=30)
            check("the machine comes up as an ordinary user",
                  "user" in out)

            refused = q.command("set name machine", "set:", timeout=30)
            check("changing a setting is refused",
                  "needs admin" in refused)
            # A refusal that changed the setting anyway would be worse
            # than no refusal at all
            listing = q.command("set", "usage:", timeout=30)
            check("and the setting REALLY did not change",
                  re.search(r"name = thread\b", listing) is not None)

            # The console must not be the way round the rule the rest of
            # the machine keeps
            wr = q.command("write system.cfg whatever", "permission",
                           timeout=30)
            check("writing to a system file from the console is refused",
                  "no permission" in wr)

            # ...and neither must a program in ring 3: this refusal
            # comes from the kernel, through the syscall
            rm = q.command("run rm swap.sys", "ended", timeout=60)
            check("a ring 3 program is refused too",
                  "no permission" in rm)
            check("and it is a refusal, not 'there is no file'",
                  "there is no file" not in rm)
            fls = q.command("fls", "swap.sys", timeout=30)
            check("the system file is still on the disk", "swap.sys" in fls)

            # --- the password ---------------------------------------
            q.type_line("sudo set name machine")
            time.sleep(1)
            bad = q.command("invalid", "sudo:", timeout=30)
            check("a wrong password opens nothing", "wrong password" in bad)
            listing = q.command("set", "usage:", timeout=30)
            check("and after a wrong password the setting is still the old one",
                  re.search(r"name = thread\b", listing) is not None)

            out = sudo(q, "set name machine", "thread", "saved")
            check("the right password runs the command as admin",
                  "name = machine (saved)" in out)

            # One command, not a session
            out = q.command("whoami", "user", timeout=30)
            check("after the command we are an ordinary user again",
                  "user" in out)

            # --- an admin session, and the way out of it -------------
            q.type_line("sudo")
            time.sleep(1)
            out = q.command("thread", "admin", timeout=30)
            check("'sudo' with no command gives an admin session",
                  "you are admin" in out)
            out = q.command("whoami", "admin", timeout=30)
            check("and 'whoami' confirms it", "admin (" in out)
            out = q.command("set color 14", "saved", timeout=30)
            check("in an admin session the settings change with no question",
                  "color = 14 (saved)" in out)
            out = q.command("logout", "user", timeout=30)
            check("'logout' ends the session", "an ordinary user" in out)
            out = q.command("set color 9", "set:", timeout=30)
            check("and after the logout it is forbidden again",
                  "needs admin" in out)

            # --- three wrong answers in a row ------------------------
            # A six-character password is an afternoon's work for a
            # program that may ask as fast as it likes
            for _ in range(3):
                q.type_line("sudo set color 9")
                time.sleep(0.8)
                q.command("wrongpassword", "sudo:", timeout=30)
            q.type_line("sudo set color 9")
            time.sleep(0.8)
            out = q.command("thread", "sudo:", timeout=30)
            check("after three wrong attempts even the right password does not work",
                  "wrong password" in out)
            time.sleep(7)               # the lock is about five seconds
            out = sudo(q, "set color 9", "thread", "saved", timeout=40)
            check("after a moment the right password works again",
                  "color = 9 (saved)" in out)

            # --- changing the password -------------------------------
            q.type_line("sudo passwd")
            time.sleep(1)
            q.type_line("thread")       # for sudo itself
            time.sleep(1)
            q.type_line("newpassword")    # the new one
            time.sleep(0.8)
            out = q.command("newpassword", "passwd", timeout=30)
            check("password changed", "password changed" in out)

            # The file holds a number, not the password
            f = q.command("cat system.cfg", "password=", timeout=30)
            check("the file on the disk holds no password in the clear",
                  "newpassword" not in f)
            check("only a number computed from it",
                  re.search(r"password=\d{6,}", f) is not None)
        finally:
            q.close()

        print("--- after a restart on the same disk ---")
        slog2 = tempfile.mktemp(suffix=".log")
        q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog2)
        try:
            q2.wait_for("ThreadOS shell", timeout=40)
            q2.type_line("sudo set color 12")
            time.sleep(1)
            out = q2.command("thread", "sudo:", timeout=30)
            check("the old password no longer works after the restart",
                  "wrong password" in out)
            time.sleep(1)
            out = sudo(q2, "set color 12", "newpassword", "saved",
                       timeout=40)
            check("the new password survived the restart",
                  "color = 12 (saved)" in out)
        finally:
            q2.close()
            if os.path.exists(slog2):
                os.unlink(slog2)
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
