#!/usr/bin/env python3
"""ThreadOS smoke test: long paths, and listings that leave the process
where it stands.

Every path a program gave the kernel was cut at 63 bytes in all, and the
working directory's path was put together in a 252-byte buffer on the
kernel's stack with nothing to stop a deep chain of names running past
it. Both are the first step towards TFS4 (docs/tfs4-plan.md, stage 1):

  user64/longpath64 (straight through usys64.h): eight directories of
  40-byte names made by whole paths, a file at the bottom written,
  stat'ed, renamed and read back by whole paths of over 300 bytes, the
  directory listed by its path (SYS64_READDIR2) without going there,
  chdir and getcwd by the whole path, all of it removed again; then a
  working directory 427 bytes deep, reached in steps any kernel allows,
  and getcwd asked where it is

  libc/tests/pathtest (musl): the same through mkdir, fopen, stat,
  fstat, rename, chdir, getcwd, unlink and rmdir - and opendir and stat
  of a directory in one thread while another opens a relative path:
  those used to move the whole process into the directory for a moment,
  and the other thread's opens landed there

Every line the programs print is checked, and the kernel must still be
answering afterwards. (The old getcwd wrote a 427-byte path into its
252-byte buffer; that happened to go unnoticed in a test run, which is
no reason to trust it.)

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64

PROMPT = "thread64>"

LONGPATH = [
    "made a chain of directories, the deepest by a path of bytes 327",
    "wrote a file by a path of bytes 340",
    "stat by the whole path gives the size 50",
    "renamed by whole paths and read back, bytes 50",
    "listed a directory by a path of bytes 286",
    "chdir by the whole path, and getcwd says it, bytes 328",
    "removed it all by whole paths, directories 8",
    "getcwd of a working directory reached step by step, bytes 427",
    "all done",
]

PATHTEST = [
    "mkdir down a chain of six 50-byte names",
    "fopen and write by a path of 316 bytes",
    "stat by the long path: a regular file of the right size",
    "stat of the deepest directory says directory",
    "fstat and stat give one inode number",
    "rename between long paths",
    "the renamed file reads back",
    "chdir by a long path",
    "getcwd says the same path",
    "and back to the root",
    "unlink and rmdir all of it by long paths",
    "and every listing saw the subdirectory's file",
    "the working directory is where it was",
]


def lines_of(out, tag):
    return [l.strip()[len(tag):].strip() for l in out.splitlines()
            if l.strip().startswith(tag)]


def main():
    print("--- long paths, and listings that leave the process put ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    disk = tempfile.mktemp(suffix=".img")
    slog = tempfile.mktemp(suffix=".log")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
        try:
            q.wait_serial(PROMPT, timeout=60)
            out = q.command("run longpath64", "exit code", timeout=90)
            said = lines_of(out, "[longpath64]")
            for want in LONGPATH:
                check("longpath64: " + want, want in said)
            bad = [l for l in said if l.startswith("ERROR")]
            check("longpath64 found nothing wrong (%s)" % bad, not bad)
            check("longpath64 ended with code 0",
                  "exit code 0" in out or "exit code: 0" in out)

            out = q.command("run pathtest", "exit code", timeout=120)
            said = lines_of(out, "[pathtest]")
            for want in PATHTEST:
                check("pathtest: " + want, "ok: " + want in said)
            race = [l for l in said if "relative opens missed" in l]
            check("pathtest: a thread listing a subdirectory never moved "
                  "another's relative opens (%s)"
                  % (race[0] if race else "no line"),
                  bool(race) and race[0].startswith("ok:"))
            fails = [l for l in said if l.startswith("FAIL")]
            check("pathtest found nothing wrong (%s)" % fails, not fails)
            check("pathtest ended with 'all done'", "all done" in said)

            out = q.command("pwd", PROMPT, timeout=20)
            check("the kernel is still answering afterwards ('%s')"
                  % out.strip().splitlines()[-1:],
                  "/" in out)
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
