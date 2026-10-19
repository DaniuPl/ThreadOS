#!/usr/bin/env python3
"""ThreadOS smoke test: account sessions and private home directories.

An account's identity has to survive the way to the filesystem, not only
the login screen: the owner can create and read a private file, another
ordinary account cannot, and an administrator can manage accounts after
reauthenticating. The same disk is booted again to check that credentials
and home files persist. The group runs against both kernels when available.
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


def run_kernel(kernel, initrd, qemu, label):
    mmap_app = "mmapzap64" if label == "64-bit" else "mmapzap"
    disk = fresh_disk()
    log = tempfile.mktemp(suffix=".log")
    logs = [log]
    workdir = iso = None
    try:
        if qemu == "qemu-system-x86_64":
            workdir = tempfile.mkdtemp()
            iso = tempfile.mktemp(suffix=".iso")
            if not make_iso64(workdir, iso):
                check(f"{label}: the bootable test disc builds", False)
                return
            boot_text = "thread64>"
        else:
            boot_text = "ThreadOS shell"

        def boot(serial_path):
            if iso:
                return Qemu(KERNEL, INITRD, disk, cdrom=iso,
                            serial=serial_path, qemu=qemu, sata=True)
            return Qemu(kernel, initrd, disk, append=TEXT_MODE,
                        serial=serial_path, qemu=qemu)

        q = boot(log)
        try:
            q.wait_serial(boot_text, timeout=90)

            out = q.command("whoami", "guest", timeout=30)
            check(f"{label}: the maintenance shell starts without an account",
                  "guest (user)" in out)

            q.command("login admin", "password:", timeout=20)
            out = answer(q, "thread", "logged in as admin")
            check(f"{label}: the administrator account accepts its password",
                  "logged in as admin" in out)
            out = q.command("pwd", "/home/admin", timeout=20)
            check(f"{label}: login starts in the admin home directory",
                  "/home/admin" in out)

            # A plain session belonging to UID 0 must not regain machine
            # write access by spelling a protected path through '..'.
            out = q.command("logout", "ordinary user again", timeout=20)
            check(f"{label}: logout drops the admin role",
                  "ordinary user again" in out)
            out = q.command("write /home/admin/../../system.cfg tampered",
                            "could not write", timeout=20)
            check(f"{label}: normalized paths cannot bypass system-file ownership",
                  "could not write" in out)
            q.command("login admin", "password:", timeout=20)
            out = answer(q, "thread", "logged in as admin")
            check(f"{label}: the administrator can authenticate again",
                  "logged in as admin" in out)

            # Account changes are risky, so even an already authenticated
            # administrator must prove the password again.
            q.command("useradd alice", "admin password:", timeout=20)
            q.command("wrongpass", "new account password:", timeout=20)
            q.command("unusedpass", "again:", timeout=20)
            out = answer(q, "unusedpass", "could not create account")
            check(f"{label}: a wrong admin password blocks account creation",
                  "could not create account" in out)

            q.command("useradd alice", "admin password:", timeout=20)
            q.command("thread", "new account password:", timeout=20)
            q.command("alicepass", "again:", timeout=20)
            out = answer(q, "alicepass", "account created")
            check(f"{label}: a fresh admin password allows account creation",
                  "account created" in out)

            out = q.command("write admin.txt ADMIN_PRIVATE", timeout=20)
            check(f"{label}: the admin can create a file in the home folder",
                  "wrote" in out or "written" in out)

            q.command("login alice", "password:", timeout=20)
            out = answer(q, "alicepass", "logged in as alice")
            check(f"{label}: the new account can sign in",
                  "logged in as alice" in out)
            out = q.command("pwd", "/home/alice", timeout=20)
            check(f"{label}: each account gets its own home directory",
                  "/home/alice" in out)
            out = q.command(f"run {mmap_app} system.cfg probe", "has ended",
                            timeout=30)
            check(f"{label}: a plain user cannot map system settings for writing",
                  "a plain user cannot map the file for writing" in out)

            out = q.command("cat /home/admin/admin.txt", "there is no file",
                            timeout=20)
            check(f"{label}: Alice cannot read the admin's private file",
                  "ADMIN_PRIVATE" not in out and "there is no file" in out)
            out = q.command("write alice.txt ALICE_PRIVATE", timeout=20)
            check(f"{label}: Alice can create a private home file",
                  "wrote" in out or "written" in out)

            q.command("passwd", "current password:", timeout=20)
            q.command("alicepass", "new password:", timeout=20)
            q.command("alicepass2", "again:", timeout=20)
            out = answer(q, "alicepass2", "password changed")
            check(f"{label}: an ordinary user can change their own password",
                  "password changed" in out)

            # A user may elevate with an administrator's password, but
            # that does not make the user's own password a valid second
            # check for account changes.
            q.command("sudo", "password:", timeout=20)
            out = answer(q, "thread", "you are admin")
            check(f"{label}: Alice can open a persistent admin session",
                  "you are admin" in out)
            out = q.command(f"run {mmap_app} system.cfg privdrop", "has ended",
                            timeout=30)
            check(f"{label}: dropping admin blocks writes through an existing mapping",
                  "a dropped admin cannot flush the file" in out)
            out = q.command("cat system.cfg", "background=", timeout=20)
            check(f"{label}: the dropped admin left system settings unchanged",
                  "name=" in out and "Xame=" not in out)
            q.command("useradd bob", "admin password:", timeout=20)
            q.command("alicepass2", "new account password:", timeout=20)
            q.command("bobpass", "again:", timeout=20)
            out = answer(q, "bobpass", "could not create account")
            check(f"{label}: Alice's password cannot reauthenticate as admin",
                  "could not create account" in out)
            q.command("useradd bob", "admin password:", timeout=20)
            q.command("thread", "new account password:", timeout=20)
            q.command("bobpass", "again:", timeout=20)
            out = answer(q, "bobpass", "account created")
            check(f"{label}: the administrator password reauthenticates account changes",
                  "account created" in out)

            q.command("login admin", "password:", timeout=20)
            out = answer(q, "thread", "logged in as admin")
            check(f"{label}: the admin can sign back in", "logged in as admin" in out)
            out = q.command("cat /home/alice/alice.txt", "ALICE_PRIVATE",
                            timeout=20)
            check(f"{label}: an administrator can access Alice's file",
                  "ALICE_PRIVATE" in out)
        finally:
            q.close()

        log2 = tempfile.mktemp(suffix=".log")
        logs.append(log2)
        q = boot(log2)
        try:
            q.wait_serial(boot_text, timeout=90)
            q.command("login alice", "password:", timeout=20)
            out = answer(q, "alicepass", "invalid account or password")
            check(f"{label}: the old password stopped working after the change",
                  "invalid account or password" in out)
            q.command("login alice", "password:", timeout=20)
            out = answer(q, "alicepass2", "logged in as alice")
            check(f"{label}: account credentials persist across reboot",
                  "logged in as alice" in out)
            out = q.command("cat alice.txt", "ALICE_PRIVATE", timeout=20)
            check(f"{label}: home files persist and remain available to owner",
                  "ALICE_PRIVATE" in out)

            q.command("login admin", "password:", timeout=20)
            out = answer(q, "thread", "logged in as admin")
            check(f"{label}: an administrator can return to account management",
                  "logged in as admin" in out)
            q.command("userdel alice", "admin password:", timeout=20)
            out = answer(q, "thread", "home directory was archived")
            check(f"{label}: deleting an account archives its private home",
                  "home directory was archived" in out)
            q.command("useradd alice", "admin password:", timeout=20)
            q.command("thread", "new account password:", timeout=20)
            q.command("freshpass", "again:", timeout=20)
            out = answer(q, "freshpass", "account created")
            check(f"{label}: a deleted account name can be reused",
                  "account created" in out)
            q.command("login alice", "password:", timeout=20)
            out = answer(q, "freshpass", "logged in as alice")
            check(f"{label}: the recreated account gets a new login",
                  "logged in as alice" in out)
            out = q.command("pwd", "/home/alice", timeout=20)
            check(f"{label}: the recreated account has a fresh home directory",
                  "/home/alice" in out)
            out = q.command("cat alice.txt", "there is no file", timeout=20)
            check(f"{label}: a recreated account cannot read the old owner's files",
                  "there is no file" in out and "ALICE_PRIVATE" not in out)
        finally:
            q.close()
    finally:
        for path in [disk, iso] + logs:
            if os.path.exists(path):
                os.unlink(path)
        if workdir and os.path.isdir(workdir):
            shutil.rmtree(workdir, ignore_errors=True)


def main():
    run_kernel(KERNEL, INITRD, "qemu-system-i386", "32-bit")
    if KERNEL64 and os.path.exists(KERNEL64):
        run_kernel(KERNEL64, INITRD64, "qemu-system-x86_64", "64-bit")
    else:
        print("SKIPPED: no 64-bit kernel image")
    return summary()


sys.exit(main())
