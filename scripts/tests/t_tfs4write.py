#!/usr/bin/env python3
"""ThreadOS smoke test: what TFS4 writes, judged by Linux's e2fsck.

TFS4 is the ext2 format (docs/tfs4-plan.md); this is its stage 3,
writing. A blank disk is formatted by the kernel ('format4'), then
libc/tests/fswrite does a day's writing on it through musl - 5000 files
made in one directory and every odd one removed, fifty moved into
another directory which is itself moved under a third, a file written
into its doubly indirect blocks with a hole before, cut back, grown by
ftruncate and written at its end, a file removed while still open -
and sqlite3 builds a database (2000 rows, a third deleted, VACUUM).
fswalk lists what ThreadOS sees, with a SHA-256 of every file.

Then the disk, on the host:

  e2fsck -fn (Linux's checker, the independent implementation) finds
  nothing it would fix - its exit code alone is not enough, as it says
  0 for a wrong free count in the superblock, so its report is read
  dumpe2fs: revision 1, the features the kernel's mkfs promises, the
  label ThreadOS
  debugfs copies the tree out, and what Linux reads is what ThreadOS
  listed, file by file, and what fswrite was asked to write - every
  file's bytes known from its name
  Python's sqlite3 opens the database: integrity ok, the rows left

Run on its own or through scripts/run_tests.py.
"""

import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64
from t_tfs4read import expected

PROMPT = "thread64>"
FILES = 5000


def body(n):
    return ("file %d\n" % n).encode() * (n % 13 + 1)


def pattern(off):
    return (off * 131 + off // 4096) & 0xFF


def grow_bin():
    head = bytes(pattern(i) for i in range(100 << 10))
    return (head + bytes(100 << 10) +
            b"the end of a file grown by ftruncate\n")


def e2fsck(img, work):
    """e2fsck -fn's report and exit code. The guest's clock is fixed in
    2030 (ptest.RTC_BASE), so the file system's times lie ahead of the
    host's; e2fsck is told the clock is not to be trusted rather than
    left to call every time stamp an error."""
    conf = os.path.join(work, "e2fsck.conf")
    with open(conf, "w") as f:
        f.write("[options]\n\tbroken_system_clock = 1\n")
    env = dict(os.environ, E2FSCK_CONFIG=conf)
    run = subprocess.run(["e2fsck", "-fn", img], capture_output=True,
                         text=True, env=env)
    return run.returncode, run.stdout + run.stderr


def main():
    print("--- what TFS4 writes, judged by e2fsck ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    for tool in ("e2fsck", "debugfs", "dumpe2fs"):
        if not shutil.which(tool):
            check("%s (e2fsprogs) is installed - the test needs it" % tool,
                  False)
            return summary()
    work = tempfile.mkdtemp()
    img = os.path.join(work, "disk.img")
    slog = os.path.join(work, "s.log")
    try:
        subprocess.run(["truncate", "-s", "256M", img], check=True)
        q = Qemu(KERNEL, INITRD, img, sata=True, append=TEXT_MODE,
                 serial=slog)
        try:
            q.wait_serial(PROMPT, timeout=90)
            q.command("sudo format4", "password:", timeout=20)
            q.command("thread", "Type YES", timeout=20)
            out = q.command("YES", PROMPT, timeout=120)
            check("'format4' makes the disk TFS4 and mounts it ('%s')"
                  % next((l.strip() for l in out.splitlines()
                          if "formatted" in l), "no line"),
                  "TFS4: formatted" in out and "is a ThreadOS TFS4 now" in out)

            out = q.command("run fswrite", "exit code", timeout=900)
            said = [l.strip()[len("[fswrite]"):].strip()
                    for l in out.splitlines()
                    if l.strip().startswith("[fswrite]")]
            for want in ("directories made",
                         "5000 files made in one directory",
                         "every odd one removed",
                         "fifty moved into another directory",
                         "and that directory moved under a third",
                         "the moved directory's '..' leads to its new parent",
                         "a file written past its direct and into its "
                         "doubly indirect blocks",
                         "cut back to 100 KiB, grown to 200 KiB, written at "
                         "its end",
                         "a file removed while open is still written and read",
                         "and gone by its name",
                         "the database's SQL written"):
                check("fswrite: " + want, "ok: " + want in said)
            check("fswrite: done", "done" in said)

            out = q.command("run sqlite3 -init /w/build.sql /w/test.db .quit",
                            "exit code", timeout=600)
            check("sqlite3 builds its database on TFS4",
                  "exit code 0" in out)

            out = q.command("run fswalk /w", "END dirs=", timeout=300)
            out += q.wait_serial(PROMPT, timeout=30)
            seen = [l.strip() for l in out.splitlines()
                    if l.strip()[:2] in ("D ", "F ", "M ")]
        finally:
            q.close()

        code, report = e2fsck(img, work)
        problems = [l for l in report.splitlines() if "?" in l]
        check("e2fsck -fn finds nothing to fix (exit %d, %s)"
              % (code, problems[:3]), code == 0 and not problems)
        if code or problems:
            print("\n".join("   | " + l for l in report.splitlines()[-25:]))

        dump = subprocess.run(["dumpe2fs", "-h", img], capture_output=True,
                              text=True).stdout
        feats = next((l.split(":", 1)[1].split() for l in dump.splitlines()
                      if l.startswith("Filesystem features:")), [])
        check("dumpe2fs: revision 1 with the features mkfs promises (%s)"
              % " ".join(feats),
              "1 (dynamic)" in dump and
              sorted(feats) == ["filetype", "large_file", "sparse_super"])
        check("and the label ThreadOS",
              any(l.startswith("Filesystem volume name:") and
                  l.split(":", 1)[1].strip() == "ThreadOS"
                  for l in dump.splitlines()))

        out = os.path.join(work, "out")
        os.mkdir(out)
        subprocess.run(["debugfs", "-R", "rdump /w " + out, img],
                       capture_output=True, check=True)
        tree = os.path.join(out, "w")
        lines = []
        for l in expected(tree):         # paths from the tree's top: /w
            kind, rest = l[0], l[2:]
            if kind == "D":
                lines.append("D /w" + ("" if rest == "/" else rest))
            elif kind == "M":
                lines.append("M /w" + rest)
            else:
                size, digest, path = rest.split(" ", 2)
                lines.append("F %s %s /w%s" % (size, digest, path))
        missing = [l for l in lines if l not in seen]
        check("what Linux reads is what ThreadOS listed (%d of %d lines "
              "differ: %s)" % (len(missing), len(lines),
                               [m[:80] for m in missing[:3]]),
              lines and not missing)

        a = os.path.join(tree, "a")
        names = sorted(os.listdir(a))
        want = ["f%04d" % n for n in range(100, FILES, 2)]
        check("Linux finds the 2450 files that stayed, by name (%d)"
              % len(names), names == want)
        bad = [n for n in want
               if open(os.path.join(a, n), "rb").read() != body(int(n[1:]))]
        check("and each holds what fswrite wrote (%d differ)" % len(bad),
              not bad)
        moved = sorted(os.listdir(os.path.join(tree, "c", "b")))
        check("the fifty moved files, in the moved directory",
              moved == ["moved-%04d" % n for n in range(0, 100, 2)] and
              all(open(os.path.join(tree, "c", "b", m), "rb").read() ==
                  body(int(m[6:])) for m in moved))
        check("no directory left where it was moved from",
              not os.path.exists(os.path.join(tree, "b")))
        with open(os.path.join(tree, "grow.bin"), "rb") as f:
            got = f.read()
        check("the grown and cut file: its head, a hole of zeros, its tail "
              "(%d bytes)" % len(got), got == grow_bin())
        check("the file removed while open is gone",
              not os.path.exists(os.path.join(tree, "tmpfile")))

        db = sqlite3.connect(os.path.join(tree, "test.db"))
        ok = db.execute("PRAGMA integrity_check").fetchone()[0]
        rows = db.execute("SELECT count(*), sum(id), "
                          "sum(length(pad)) FROM t").fetchone()
        db.close()
        ids = [i for i in range(1, 2001) if i % 3]
        check("Python's sqlite3 opens the database: integrity %s, %d rows"
              % (ok, rows[0]),
              ok == "ok" and rows == (len(ids), sum(ids),
                                      sum(100 + i % 400 for i in ids)))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
