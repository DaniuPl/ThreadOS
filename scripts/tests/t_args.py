#!/usr/bin/env python3
"""ThreadOS test: long command lines, and programs named by their path.

A compiler hands its linker thirty paths and finds that linker by a
path of its own (/usr/bin/ld.lld). Both were lost on the way into the
kernel: sixteen arguments of 255 characters reached a program and the
rest were dropped without a word, and execve passed on only the last
part of a path, which the kernel looked for in the initrd and in the
working directory. kernel64/proc64.c now copies a program's arguments
packed, up to 64 KiB, and refuses more with E2BIG; kernel/prog.c looks
a path up as the path it is.

The disk is made on this machine (mke2fs -d) with argtest
(libc/tests/argtest.c) at its root and a second copy of it at
/usr/libexec/threados/argecho - a name neither in the initrd nor where
the shell stands. Booted on it:

  argtest starts argecho by that path with 301 arguments, 11 KB of
  them, one 3000 characters long; argecho says how many it got, how
  many bytes and their FNV-1a hash, and this test works out the same
  three from the rule they were made by

  70 KB of arguments are refused with E2BIG, and argtest goes on

  the kernel's shell hands a program more than sixteen words

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

BUILD = os.path.dirname(os.path.abspath(KERNEL64 or "build/x"))
ELF = os.path.join(BUILD, "user64", "argtest.elf")
ECHO_PATH = "/usr/libexec/threados/argecho"
PROMPT = "thread64>"


def expected():
    """argc, bytes and FNV-1a of the arguments argtest makes, made here
    by the same rule"""
    args = [ECHO_PATH]
    for i in range(1, 300):
        args.append("arg%03d-" % i + "x" * (i % 40))
    args.append("L" * 3000)
    data = b"".join(a.encode() + b"\0" for a in args)
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return len(args), len(data), "%08x" % h


def make_disk(work):
    tree = os.path.join(work, "tree")
    libexec = os.path.join(tree, "usr", "libexec", "threados")
    os.makedirs(libexec)
    shutil.copy(ELF, os.path.join(tree, "argtest"))
    shutil.copy(ELF, os.path.join(libexec, "argecho"))
    for p in (os.path.join(tree, "argtest"), os.path.join(libexec, "argecho")):
        os.chmod(p, 0o755)
    img = os.path.join(work, "disk.img")
    subprocess.run(["truncate", "-s", "32M", img], check=True)
    subprocess.run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", "4096",
                    "-L", "ThreadOS", "-d", tree, img], check=True)
    return img


def main():
    print("--- long command lines, programs by their path ---")
    if not os.path.exists(ELF):
        check("argtest.elf is built (make)", False)
        return summary()
    argc, nbytes, h = expected()
    work = tempfile.mkdtemp()
    img = make_disk(work)
    q = Qemu(KERNEL, INITRD, img, sata=True, append=TEXT_MODE,
             serial=os.path.join(work, "s.log"))
    try:
        q.wait_serial(PROMPT, timeout=90)
        out = q.command("run /argtest", "[/argtest] done, exit code",
                        timeout=120)
        out = out.replace("\r", "")

        m = re.search(r"argecho: argc=(\d+) bytes=(\d+) fnv=([0-9a-f]{8})", out)
        got = (int(m.group(1)), int(m.group(2)), m.group(3)) if m else None
        check(f"a program started by a path of {len(ECHO_PATH)} characters, "
              f"on the disk only, runs ({'yes' if m else 'no'})",
              m is not None and "argtest: started by its path: yes" in out)
        check(f"and gets all {argc} arguments, {nbytes} bytes, intact "
              f"(got {got}, want {(argc, nbytes, h)})",
              got == (argc, nbytes, h))
        check("its argv[0] is the path it was started by",
              f"argecho: argv[0]={ECHO_PATH}\n" in out)
        mine = re.search(r"argtest: argc=(\d+) bytes=(\d+) fnv=([0-9a-f]{8})",
                         out)
        check("argtest counted the same before it handed them over",
              mine is not None and
              (int(mine.group(1)), int(mine.group(2)), mine.group(3)) ==
              (argc, nbytes, h))
        check("the copy ended with code 0",
              "argtest: and it ended with 0" in out)
        check("70 KB of arguments are refused with E2BIG, not cut short",
              "refused with E2BIG: yes" in out)
        check("and argtest went on to end with code 0",
              "[/argtest] done, exit code 0" in out)

        words = " ".join("w%d" % i for i in range(1, 26))
        out = q.command("run /argtest count " + words,
                        "[/argtest] done, exit code", timeout=60)
        m = re.search(r"argtest: argc=(\d+) last=(\S+)", out)
        check("the shell hands a program 27 words, past the sixteen it "
              f"used to ({m.group(0) if m else 'nothing'})",
              m is not None and m.group(1) == "27" and m.group(2) == "w25")
    finally:
        q.close()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
