#!/usr/bin/env python3
"""ThreadOS smoke test: account sessions and private home directories.

An account's identity has to survive the way to the filesystem, not only
the login screen: the owner can create and read a private file, another
ordinary account cannot, and an administrator can manage accounts after
reauthenticating. The same disk is booted again to check that credentials
and home files persist.
"""

import os
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


def answer(q, text, want, timeout=30):
    return q.command(text, want, timeout=timeout)


def run_kernel():
    disk = fresh_disk()
    log = tempfile.mktemp(suffix=".log")
    logs = [log]
    try:
        def boot(serial_path):
            return Qemu(KERNEL, INITRD, disk, append=TEXT_MODE,
                        serial=serial_path, sata=True)

        q = boot(log)
        try:
            q.wait_serial("thread64>", timeout=90)

            out = q.command("whoami", "guest", timeout=30)
            check("the maintenance shell starts without an account",
                  "guest (user)" in out)

            q.command("login admin", "password:", timeout=20)
            out = answer(q, "thread", "logged in as admin")
            check("the administrator account accepts its password",
                  "logged in as admin" in out)
            out = q.command("pwd", "/home/admin", timeout=20)
            check("login starts in the admin home directory",
                  "/home/admin" in out)
            # A fresh disk has no settings file until a setting is saved,
            # and refusing to touch a file that is not there proves
            # nothing - so the checks on system.cfg below need one first
            out = q.command("set name thread", "saved", timeout=20)
            check("the administrator saves a setting, which makes system.cfg",
                  "name = thread (saved)" in out)

            # A plain session belonging to UID 0 must not regain machine
            # write access by spelling a protected path through '..'.
            out = q.command("logout", "ordinary user again", timeout=20)
            check("logout drops the admin role",
                  "ordinary user again" in out)
            out = q.command("write /home/admin/../../system.cfg tampered",
                            "could not write", timeout=20)
            check("normalized paths cannot bypass system-file ownership",
                  "could not write" in out)
            out = q.command("cat /system.cfg", "background=", timeout=20)
            check("and the settings file kept its contents",
                  "name=thread" in out and "tampered" not in out)
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

            out = q.command("write admin.txt ADMIN_PRIVATE", timeout=20)
            check("the admin can create a file in the home folder",
                  "wrote" in out or "written" in out)

            q.command("login alice", "password:", timeout=20)
            out = answer(q, "alicepass", "logged in as alice")
            check("the new account can sign in",
                  "logged in as alice" in out)
            out = q.command("pwd", "/home/alice", timeout=20)
            check("each account gets its own home directory",
                  "/home/alice" in out)
            out = q.command("run mmapzap64 /system.cfg probe", "done, exit code",
                            timeout=30)
            check("a plain user cannot map system settings for writing",
                  "a plain user cannot map the file for writing" in out)

            out = q.command("cat /home/admin/admin.txt", "there is no file",
                            timeout=20)
            check("Alice cannot read the admin's private file",
                  "ADMIN_PRIVATE" not in out and "there is no file" in out)
            out = q.command("write alice.txt ALICE_PRIVATE", timeout=20)
            check("Alice can create a private home file",
                  "wrote" in out or "written" in out)

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
            check("Alice can open a persistent admin session",
                  "you are admin" in out)
            out = q.command("run mmapzap64 /system.cfg privdrop", "done, exit code",
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
            check("Alice's password cannot reauthenticate as admin",
                  "could not create account" in out)
            q.command("useradd bob", "admin password:", timeout=20)
            q.command("thread", "new account password:", timeout=20)
            q.command("bobpass", "again:", timeout=20)
            out = answer(q, "bobpass", "account created")
            check("the administrator password reauthenticates account changes",
                  "account created" in out)

            q.command("login admin", "password:", timeout=20)
            out = answer(q, "thread", "logged in as admin")
            check("the admin can sign back in", "logged in as admin" in out)
            out = q.command("cat /home/alice/alice.txt", "ALICE_PRIVATE",
                            timeout=20)
            check("an administrator can access Alice's file",
                  "ALICE_PRIVATE" in out)
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
            out = q.command("cat alice.txt", "ALICE_PRIVATE", timeout=20)
            check("home files persist and remain available to owner",
                  "ALICE_PRIVATE" in out)

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
            check("the recreated account has a fresh home directory",
                  "/home/alice" in out)
            out = q.command("cat alice.txt", "there is no file", timeout=20)
            check("a recreated account cannot read the old owner's files",
                  "there is no file" in out and "ALICE_PRIVATE" not in out)
        finally:
            q.close()
    finally:
        for path in [disk] + logs:
            if os.path.exists(path):
                os.unlink(path)


def main():
    run_kernel()
    return summary()


sys.exit(main())
