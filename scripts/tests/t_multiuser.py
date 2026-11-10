#!/usr/bin/env python3
"""ThreadOS smoke test: account sessions and private home directories.

An account's identity has to survive the way to the filesystem, not only
the login screen: the owner can create and read a private file, another
ordinary account cannot, and an administrator can manage accounts after
reauthenticating. The same disk is booted again to check that credentials
and home files persist.
"""

import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def fresh_disk():
    path = tempfile.mktemp(suffix=".img")
    with open(path, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    return path


def answer(q, text, want, timeout=30):
    return q.command(text, want, timeout=timeout)


def run():
    disk = fresh_disk()
    log = tempfile.mktemp(suffix=".log")
    logs = [log]
    workdir = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    try:
        if not make_iso64(workdir, iso):
            check("the bootable test disc builds", False)
            return

        def boot(serial_path):
            return Qemu(KERNEL, INITRD, disk, cdrom=iso,
                        serial=serial_path, sata=True)

        q = boot(log)
        try:
            q.wait_serial("thread64>", timeout=90)

            out = q.command("whoami", "guest", timeout=30)
            check("the maintenance shell starts without an account",
                  "guest (user)" in out)

            # A fresh disk: nobody has set up a profile, and this login
            # with the bootstrap password is the first use of the account
            q.command("login admin", "password:", timeout=20)
            out = answer(q, "thread", "logged in as admin")
            check("the administrator account accepts its password",
                  "logged in as admin" in out)
            out = q.command("pwd", "/home/admin", timeout=20)
            check("login starts in the admin home directory",
                  "/home/admin" in out)

            # A plain session belonging to UID 0 must not regain machine
            # write access by spelling a protected path through '..'.
            out = q.command("logout", "ordinary user again", timeout=20)
            check("logout drops the admin role",
                  "ordinary user again" in out)
            out = q.command("whoami", "(", timeout=20)
            plain_admin = "admin (user)" in out
            out = q.command("write /home/admin/../../system.cfg tampered",
                            "could not write", timeout=20)
            check("normalized paths cannot bypass system-file ownership",
                  plain_admin and "could not write" in out)
            q.command("login admin", "password:", timeout=20)
            out = answer(q, "thread", "logged in as admin")
            check("the administrator can authenticate again",
                  "logged in as admin" in out)

            # Account changes are risky, so even an already authenticated
            # administrator must prove the password again.
            q.command("useradd alice", "admin password:", timeout=20)
            q.command("wrongpass", "new account password:", timeout=20)
            q.command("unusedpass", "again:", timeout=20)
            out = answer(q, "unusedpass", "could not create account")
            check("a wrong admin password blocks account creation",
                  "could not create account" in out)

            q.command("useradd alice", "admin password:", timeout=20)
            q.command("thread", "new account password:", timeout=20)
            q.command("alicepass", "again:", timeout=20)
            out = answer(q, "alicepass", "account created")
            check("a fresh admin password allows account creation",
                  "account created" in out)

            # Written by a bare name, read back by the full one: 'written'
            # alone would also be said for a file that went into whatever
            # directory the shell happened to stand in
            out = q.command("write admin.txt ADMIN_PRIVATE", "write:",
                            timeout=20)
            back = q.command("cat /home/admin/admin.txt", "ADMIN_PRIVATE",
                             timeout=20)
            admin_file = "ADMIN_PRIVATE" in back
            check("the admin can create a file in the home folder",
                  "'admin.txt' written" in out and admin_file)

            q.command("login alice", "password:", timeout=20)
            out = answer(q, "alicepass", "logged in as alice")
            check("the new account can sign in",
                  "logged in as alice" in out)
            out = q.command("pwd", "/home/alice", timeout=20)
            alice_home = "/home/alice" in out
            check("each account gets its own home directory", alice_home)
            # The settings file by its full name: a bare name is looked
            # up in the working directory, which is Alice's home now -
            # and a file that is not there cannot be mapped by anybody,
            # so the refusal would prove nothing. The probe cannot tell
            # the two apart, so the file is shown to be there first.
            out = q.command("cat /system.cfg", "name=", timeout=20)
            settings = "name=" in out and "there is no file" not in out
            check("the settings file the probe maps is there", settings)
            out = q.command("run mmapzap64 /system.cfg probe", "exit code",
                            timeout=30)
            check("a plain user cannot map system settings for writing",
                  settings and
                  "a plain user cannot map the file for writing" in out)

            # Refused only means something about a file that is there,
            # asked for by Alice
            out = q.command("cat /home/admin/admin.txt", "there is no file",
                            timeout=20)
            check("Alice cannot read the admin's private file",
                  admin_file and alice_home and
                  "ADMIN_PRIVATE" not in out and "there is no file" in out)
            out = q.command("write alice.txt ALICE_PRIVATE", "write:",
                            timeout=20)
            back = q.command("cat /home/alice/alice.txt", "ALICE_PRIVATE",
                             timeout=20)
            check("Alice can create a private home file",
                  "'alice.txt' written" in out and "ALICE_PRIVATE" in back)

            q.command("passwd", "current password:", timeout=20)
            q.command("alicepass", "new password:", timeout=20)
            q.command("alicepass2", "again:", timeout=20)
            out = answer(q, "alicepass2", "password changed")
            check("an ordinary user can change their own password",
                  "password changed" in out)

            # A user may elevate with an administrator's password, but
            # that does not make the user's own password a valid second
            # check for account changes.
            q.command("sudo", "password:", timeout=20)
            out = answer(q, "thread", "you are admin")
            who = q.command("whoami", "(", timeout=20)
            alice_admin = "alice (admin)" in who
            check("Alice can open a persistent admin session",
                  "you are admin" in out and alice_admin)
            out = q.command("run mmapzap64 /system.cfg privdrop", "exit code",
                            timeout=30)
            check("dropping admin blocks writes through an existing mapping",
                  "a dropped admin cannot flush the file" in out)
            out = q.command("cat /system.cfg", "background=", timeout=20)
            check("the dropped admin left system settings unchanged",
                  "name=" in out and "Xame=" not in out)
            q.command("useradd bob", "admin password:", timeout=20)
            q.command("alicepass2", "new account password:", timeout=20)
            q.command("bobpass", "again:", timeout=20)
            out = answer(q, "bobpass", "could not create account")
            # Only Alice's own session puts her password to the test: a
            # session of nobody's would refuse any password but the admin's
            check("Alice's password cannot reauthenticate as admin",
                  alice_admin and "could not create account" in out)
            q.command("useradd bob", "admin password:", timeout=20)
            q.command("thread", "new account password:", timeout=20)
            q.command("bobpass", "again:", timeout=20)
            out = answer(q, "bobpass", "account created")
            check("the administrator password reauthenticates account changes",
                  "account created" in out)

            q.command("login admin", "password:", timeout=20)
            out = answer(q, "thread", "logged in as admin")
            admin_back = "logged in as admin" in out
            check("the admin can sign back in", admin_back)
            # Alice's own session, still open if the login failed, could
            # read the file too
            out = q.command("cat /home/alice/alice.txt", "ALICE_PRIVATE",
                            timeout=20)
            check("an administrator can access Alice's file",
                  admin_back and "ALICE_PRIVATE" in out)

            # The right password for an account whose home folder is
            # gone: that is not a wrong password, and both the kernel's
            # log and the shell's answer say what it really is. Bob's
            # home is still empty, so it can simply be removed. The
            # shell's line comes after the kernel's, so it is the one
            # waited for.
            out = q.command("rm /home/bob", "rm:", timeout=20)
            check("the admin can remove an empty home folder",
                  "'/home/bob' removed" in out)
            q.command("login bob", "password:", timeout=20)
            out = answer(q, "bobpass", "its home folder cannot be opened")
            check("the kernel says the home folder, not the password, is "
                  "what stops the sign-in",
                  "the password is right, but the home folder /home/bob "
                  "cannot be opened" in out and "logged in as bob" not in out)
            check("and so does the shell, without calling the password "
                  "wrong",
                  "login: 'bob' cannot sign in - its home folder cannot be "
                  "opened" in out
                  and "invalid account or password" not in out)
        finally:
            q.close()

        log2 = tempfile.mktemp(suffix=".log")
        logs.append(log2)
        q = boot(log2)
        try:
            q.wait_serial("thread64>", timeout=90)
            q.command("login alice", "password:", timeout=20)
            out = answer(q, "alicepass", "invalid account or password")
            check("the old password stopped working after the change",
                  "invalid account or password" in out)
            q.command("login alice", "password:", timeout=20)
            out = answer(q, "alicepass2", "logged in as alice")
            check("account credentials persist across reboot",
                  "logged in as alice" in out)
            # By the bare name, from where the login put her
            where = q.command("pwd", "/home/alice", timeout=20)
            out = q.command("cat alice.txt", "ALICE_PRIVATE", timeout=20)
            check("home files persist and remain available to owner",
                  "/home/alice" in where and "ALICE_PRIVATE" in out)

            q.command("login admin", "password:", timeout=20)
            out = answer(q, "thread", "logged in as admin")
            check("an administrator can return to account management",
                  "logged in as admin" in out)
            q.command("userdel alice", "admin password:", timeout=20)
            out = answer(q, "thread", "home directory was archived")
            check("deleting an account archives its private home",
                  "home directory was archived" in out)
            q.command("useradd alice", "admin password:", timeout=20)
            q.command("thread", "new account password:", timeout=20)
            q.command("freshpass", "again:", timeout=20)
            out = answer(q, "freshpass", "account created")
            check("a deleted account name can be reused",
                  "account created" in out)
            q.command("login alice", "password:", timeout=20)
            out = answer(q, "freshpass", "logged in as alice")
            check("the recreated account gets a new login",
                  "logged in as alice" in out)
            out = q.command("pwd", "/home/alice", timeout=20)
            fresh_home = "/home/alice" in out
            check("the recreated account has a fresh home directory",
                  fresh_home)
            # Anywhere but in the new home, 'there is no file' would say
            # nothing about the old owner's files
            out = q.command("cat alice.txt", "there is no file", timeout=20)
            check("a recreated account cannot read the old owner's files",
                  fresh_home and
                  "there is no file" in out and "ALICE_PRIVATE" not in out)
        finally:
            q.close()
    finally:
        for path in [disk, iso] + logs:
            if os.path.exists(path):
                os.unlink(path)
        shutil.rmtree(workdir, ignore_errors=True)


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    run()
    return summary()


sys.exit(main())
