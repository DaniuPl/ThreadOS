#!/usr/bin/env python3
"""ThreadOS smoke test: POSIX's rights and metadata on TFS4.

TFS4 is the ext2 format (docs/tfs4-plan.md); this is its stage 4, the
metadata TFS3 never kept. A disk formatted with 'format4', two accounts,
and libc/tests/permtest run as each in turn:

  alice makes a directory, a 0600 secret, a 0644 file, a hard link, a
  short and a long symbolic link, a file under umask 077, sets a file's
  times with utimensat, puts a file in the root (1777, sticky)
  bob may not read the secret (EACCES, also through the link), write
  the other file, remove it, chmod it (EPERM), chown it, look into
  alice's 0700 home, or remove her file from the sticky root (EPERM);
  he may read the open file and make his own in the root
  alice opens the secret with chmod 0644; then bob reads it

Every "ok" line is checked. Then the disk, on the host: e2fsck -fn finds
nothing to fix, and debugfs (Linux's reading) says for every file what
stat said in ThreadOS - type, mode, owner, group, links, size and
modification time - and the symbolic links point where ThreadOS made
them point (one kept in the inode, one in a block).

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64
from t_tfs4write import e2fsck

PROMPT = "thread64>"
LONG_TARGET = "/shared/" + "./" * 21 + "secret"     # permtest.c's

STEPS = {
    "alice": ["mkdir /shared 0755", "a 0600 secret",
              "a 0644 file for everybody", "a hard link to the secret",
              "a short symbolic link", "a long symbolic link",
              "a file made under umask 077",
              "utimensat sets the open file's times",
              "a file of her own in the root",
              "the secret has two names now",
              "readlink gives the short link's target",
              "and the long link's",
              "lstat says link, stat follows it to the secret",
              "umask 077 made it 0600",
              "access: she may write her file",
              "her home is hers alone (0700)"],
    "bob": ["he may not read the secret",
            "nor through its symbolic link",
            "he reads the open file",
            "but may not write it",
            "nor remove it from alice's directory",
            "nor change its mode",
            "nor give it away",
            "access says the same",
            "the link's target is his to read",
            "and stat of the secret is his to ask (its bits say 0600)",
            "he may not look into alice's home",
            "nor remove her file from the root (sticky)",
            "he makes his own there"],
    "alice2": ["she opens the secret up"],
    "bob2": ["now he reads it"],
}


def login(q, name, password):
    q.command("login " + name, "password:", timeout=20)
    out = q.command(password, "logged in as" if name else PROMPT, timeout=30)
    return "logged in as " + name in out


def debugfs_stat(img, path):
    out = subprocess.run(["debugfs", "-R", "stat " + path, img],
                         capture_output=True, text=True).stdout
    m = {}
    for key, pat in [("type", r"Type:\s+(\w+)"), ("mode", r"Mode:\s+(\d+)"),
                     ("uid", r"User:\s+(\d+)"), ("gid", r"Group:\s+(\d+)"),
                     ("size", r"Size:\s+(\d+)"), ("links", r"Links:\s+(\d+)"),
                     ("mtime", r"mtime:\s+0x([0-9a-f]+)"),
                     ("fast", r'Fast link dest: "([^"]*)"')]:
        g = re.search(pat, out)
        m[key] = g.group(1) if g else None
    return m


def main():
    print("--- POSIX's rights and metadata on TFS4 ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    for tool in ("e2fsck", "debugfs"):
        if not shutil.which(tool):
            check("%s (e2fsprogs) is installed - the test needs it" % tool,
                  False)
            return summary()
    work = tempfile.mkdtemp()
    img = os.path.join(work, "disk.img")
    try:
        subprocess.run(["truncate", "-s", "128M", img], check=True)
        q = Qemu(KERNEL, INITRD, img, sata=True, append=TEXT_MODE,
                 serial=os.path.join(work, "s.log"))
        stats = {}
        try:
            q.wait_serial(PROMPT, timeout=90)
            q.command("sudo format4", "password:", timeout=20)
            q.command("thread", "Type YES", timeout=20)
            out = q.command("YES", PROMPT, timeout=120)
            check("the disk is TFS4", "is a ThreadOS TFS4 now" in out)
            check("signed in as admin", login(q, "admin", "thread"))
            for name in ("alice", "bob"):
                q.command("useradd " + name, "admin password:", timeout=20)
                q.command("thread", "new account password:", timeout=20)
                q.command(name + "pass", "again:", timeout=20)
                out = q.command(name + "pass", "account", timeout=30)
                check("an account for " + name, "account created" in out)
            for step in ("alice", "bob", "alice2", "bob2"):
                who = step.rstrip("2")
                check("signed in as " + who, login(q, who, who + "pass"))
                out = q.command("run permtest " + step, "exit code",
                                timeout=120)
                said = [l.strip()[len("[permtest]"):].strip()
                        for l in out.splitlines()
                        if l.strip().startswith("[permtest]")]
                for want in STEPS[step]:
                    line = next((l for l in said if want in l), "no line")
                    check("%s: %s" % (step, line if "FAIL" in line else want),
                          line.startswith("ok:"))
                check("%s: done" % step, "done" in said)
                for l in out.splitlines():
                    p = l.strip().split()
                    if len(p) == 8 and p[0] == "STAT":
                        stats[p[1]] = p[2:]
        finally:
            q.close()

        code, report = e2fsck(img, work)
        problems = [l for l in report.splitlines() if "?" in l]
        check("e2fsck -fn finds nothing to fix (exit %d, %s)"
              % (code, problems[:3]), code == 0 and not problems)

        types = {0o100000: "regular", 0o040000: "directory",
                 0o120000: "symlink"}
        for path in ("/shared", "/shared/secret", "/shared/open",
                     "/shared/secret-link", "/shared/sym", "/shared/longsym",
                     "/shared/masked", "/alice-root.txt", "/bob-root.txt"):
            st = stats.get(path)
            d = debugfs_stat(img, path)
            if not st:
                check("ThreadOS said what %s is" % path, False)
                continue
            mode = int(st[0], 8)
            mine = dict(type=types.get(mode & 0o170000), mode=mode & 0o7777,
                        uid=int(st[1]), gid=int(st[2]), links=int(st[3]),
                        size=int(st[4]), mtime=int(st[5]))
            linux = dict(type=d["type"], mode=int(d["mode"] or "0", 8),
                         uid=int(d["uid"] or -1), gid=int(d["gid"] or -1),
                         links=int(d["links"] or -1),
                         size=int(d["size"] or -1),
                         mtime=int(d["mtime"] or "0", 16))
            check("%s: debugfs reads what stat said in ThreadOS (%s)"
                  % (path, mine if mine != linux else
                     "%s %o uid %d gid %d links %d" % (
                         mine["type"], mine["mode"], mine["uid"],
                         mine["gid"], mine["links"])),
                  mine == linux)
        check("the secret ends 0644, with two names",
              stats.get("/shared/secret", [""])[0] == "100644" and
              debugfs_stat(img, "/shared/secret")["links"] == "2")
        check("the open file's mtime is the one utimensat set "
              "(1000000000)",
              int(debugfs_stat(img, "/shared/open")["mtime"] or "0", 16) ==
              1000000000)
        check("umask 077 left 0600 on the disk",
              debugfs_stat(img, "/shared/masked")["mode"] == "0600")
        check("the short link is kept in its inode, pointing at 'secret'",
              debugfs_stat(img, "/shared/sym")["fast"] == "secret")
        out = os.path.join(work, "out")
        os.mkdir(out)
        subprocess.run(["debugfs", "-R", "rdump /shared " + out, img],
                       capture_output=True, check=True)
        check("the long link, in a block, reads back in Linux",
              os.readlink(os.path.join(out, "shared", "longsym")) ==
              LONG_TARGET)
        check("the homes are 0700, their owners'",
              debugfs_stat(img, "/home/alice")["mode"] == "0700" and
              debugfs_stat(img, "/home/bob")["mode"] == "0700")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
