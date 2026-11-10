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
  a program in ring 3 is refused by the KERNEL, not by the program:
  'core rm' (user/core.c) is told 'no permission', which is a different
  answer from 'there is no such file'
  the new password survives a restart, and the public settings file
  contains no account credential

The machine stays at its text shell, where nobody is signed in; what it
says is read off the serial port, from what arrived after each command.

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


def ask(q, command, timeout=30):
    """'sudo <command>', up to the line that asks for the password -
    which never appears on the screen, so what comes after it is what
    the caller waits for. With no command, a bare 'sudo'."""
    q.command(("sudo " + command).strip(), "password:", timeout=timeout)


def sudo(q, command, password, wait, timeout=30):
    """'sudo <command>', then the password on the line the shell asks
    for it on."""
    ask(q, command, timeout)
    return q.command(password, wait, timeout=timeout)


def main():
    disk = new_disk()

    print("--- an ordinary user ---")
    try:
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
        try:
            q.wait_serial("thread64>", timeout=40)

            out = q.command("whoami", ")", timeout=30)
            check("the machine comes up as an ordinary user",
                  "(user)" in out)

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
            rm = q.command("run core rm swap.sys", "[core] done, exit code",
                           timeout=60)
            check("a ring 3 program is refused too",
                  "rm: no permission for 'swap.sys'" in rm and
                  "[core] done, exit code 2" in rm)
            check("and it is a refusal, not 'there is no file'",
                  "there is no file" not in rm)
            # A plain user's listing leaves out what that user may not
            # read (fs_readdir in kernel/fs.c), swap.sys among it - so
            # it is the admin who looks
            fls = sudo(q, "fls", "thread", "Used:")
            check("the system file is still on the disk", "swap.sys" in fls)

            # --- the password ---------------------------------------
            ask(q, "set name machine")
            bad = q.command("invalid", "sudo:", timeout=30)
            check("a wrong password opens nothing", "wrong password" in bad)
            listing = q.command("set", "usage:", timeout=30)
            check("and after a wrong password the setting is still the old one",
                  re.search(r"name = thread\b", listing) is not None)

            out = sudo(q, "set name machine", "thread", "saved")
            check("the right password runs the command as admin",
                  "name = machine (saved)" in out)

            # One command, not a session
            out = q.command("whoami", ")", timeout=30)
            check("after the command we are an ordinary user again",
                  "(user)" in out)

            # --- an admin session, and the way out of it -------------
            out = sudo(q, "", "thread", "admin")
            check("'sudo' with no command gives an admin session",
                  "you are admin" in out)
            out = q.command("whoami", ")", timeout=30)
            check("and 'whoami' confirms the admin role", "(admin)" in out)
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
                sudo(q, "set color 9", "wrongpassword", "sudo:")
            out = sudo(q, "set color 9", "thread", "sudo:")
            check("after three wrong attempts even the right password does not work",
                  "wrong password" in out)
            # The lock is 500 ticks of 10 ms (kernel/user.c). A try
            # while it holds is refused without counting as a wrong one
            # (attempt_allowed), so the right password can simply be
            # tried until the lock has passed - and a lock that never
            # passes is caught by the deadline, ten times its length.
            started = time.time()
            refused = 0
            out = ""
            while time.time() < started + 50:
                ask(q, "set color 9")
                start = len(q.serial())
                q.type_line("thread")
                end = time.time() + 30
                while time.time() < end:
                    out = q.serial()[start:]
                    if "sudo: wrong password" in out or "(saved)" in out:
                        break
                    time.sleep(0.3)
                if "color = 9 (saved)" in out:
                    break
                refused += 1
                time.sleep(1)
            check("after a moment the right password works again "
                  "(after %.0f s, %d more refusals)"
                  % (time.time() - started, refused),
                  "color = 9 (saved)" in out)

            # --- changing the password -------------------------------
            ask(q, "passwd")
            q.command("thread", "current password:", timeout=30)
            q.command("thread", "new password:", timeout=30)
            q.command("newpassword", "again:", timeout=30)
            out = q.command("newpassword", "password changed", timeout=30)
            check("password changed", "password changed" in out)

            # The credential is stored only in the protected account DB.
            # The whole file is read by a program in ring 3 - the
            # shell's own 'cat' shows only its first 512 bytes, and the
            # end of the file is where a credential would sit.
            f = q.command("run ucat64 system.cfg", "[ucat64] done, exit code",
                          timeout=60)
            check("the public settings file can be read whole",
                  "background=" in f and "[ucat64] done, exit code 0" in f)
            check("the public settings file contains no account credential",
                  "password=" not in f and "newpassword" not in f)
        finally:
            q.close()

        print("--- after a restart on the same disk ---")
        q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
        try:
            # The name set through sudo is what the prompt now says. The
            # wait is for either prompt: a name that did not survive is
            # then a failed check here, not an exception that skips the
            # password checks after it.
            deadline = time.time() + 40 + q2.boot_grace()
            boot = q2.serial()
            while time.time() < deadline and \
                    not re.search(r"(thread|machine)64>", boot):
                time.sleep(0.3)
                boot = q2.serial()
            check("the name set as admin survived the restart",
                  "machine64>" in boot and "thread64>" not in boot)
            out = sudo(q2, "set color 12", "thread", "sudo:")
            check("the old password no longer works after the restart",
                  "wrong password" in out)
            out = sudo(q2, "set color 12", "newpassword", "saved",
                       timeout=40)
            check("the new password survived the restart",
                  "color = 12 (saved)" in out)
        finally:
            q2.close()
    finally:
        if os.path.exists(disk):
            os.unlink(disk)

    return summary()


sys.exit(main())
