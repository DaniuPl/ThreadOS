#!/usr/bin/env python3
"""ThreadOS smoke test: TFS4 reads what Linux's mke2fs wrote.

TFS4 is the ext2 format (docs/tfs4-plan.md); this is its stage 2,
reading. The host builds a tree, Linux's mke2fs puts it into an image
(mke2fs -d, the independent implementation), ThreadOS boots on that
image and libc/tests/fswalk lists everything through musl and prints a
SHA-256 of every file; Python's hashlib says what each should be.

The tree holds what exercises the format:

  10 000 files in one directory (folded into one line by fswalk: the
  SHA-256 of "name sha256" lines, sorted)
  directories nested 25 deep, a file at the bottom
  a name of 255 bytes of UTF-8, and a directory of 200
  a sparse file of 20 MiB with data at its start and in its middle,
  holes between
  a file of 70 MiB - on a 1 KiB image past the doubly indirect block
  (64.3 MiB), into the triply indirect one
  a symbolic link to a file, kept in the inode, and one to the deep
  file whose target is too long for the inode and lives in a block

Twice: with 4 KiB blocks and with 1 KiB blocks. Then the rule for which
ext2 is ours, on a disk with a GPT: a partition of ThreadOS's type is
mounted; the same file system in a partition of Linux's type is not -
not even labelled ThreadOS - and the disk's bytes are the same after
the boot.

Run on its own or through scripts/run_tests.py.
"""

import hashlib
import os
import random
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, make_gpt_disk

PROMPT = "thread64>"
MANY = 10000
FOLD = 200                    # libc/tests/fswalk.c's
BIG = 70 << 20
SPARSE = 20 << 20
DEEP = 25
OURS_TYPE = b"THREADOS-TFS4\x00\x00\x01"
P3_START = 2048 + 100 * 2048 + 409600      # make_gpt_disk's third partition


def build_tree(top):
    """The tree, and what fswalk should print of it"""
    os.makedirs(os.path.join(top, "many"))
    for i in range(MANY):
        with open(os.path.join(top, "many", "f%05d" % i), "wb") as f:
            f.write(("file %d\n" % i).encode() * (i % 7 + 1))
    d = top
    for i in range(DEEP):
        d = os.path.join(d, "lvl-%02d" % i)
        os.mkdir(d)
    with open(os.path.join(d, "bottom.txt"), "wb") as f:
        f.write(b"at the bottom of twenty-five directories\n")
    deep_rel = os.path.relpath(os.path.join(d, "bottom.txt"), top)
    long_dir = os.path.join(top, "d" * 200)
    os.mkdir(long_dir)
    name = "\u017c" * 127 + "x"                     # 255 bytes of UTF-8
    assert len(name.encode()) == 255
    with open(os.path.join(long_dir, name), "wb") as f:
        f.write(b"a name as long as a directory entry holds\n")
    with open(os.path.join(top, "hello.txt"), "wb") as f:
        f.write(b"hello from linux\n")
    with open(os.path.join(top, "sparse.bin"), "wb") as f:
        f.write(b"start of a sparse file\n")
        f.seek(SPARSE // 2)
        f.write(b"the middle of it\n")
        f.truncate(SPARSE)
    with open(os.path.join(top, "big.bin"), "wb") as f:
        f.write(random.Random(4).randbytes(BIG))
    os.symlink("hello.txt", os.path.join(top, "hello-link"))
    os.symlink("./" + "/./".join(deep_rel.split("/")),
               os.path.join(top, "deep-link"))
    assert len(os.readlink(os.path.join(top, "deep-link"))) > 60
    return expected(top)


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def expected(top):
    """fswalk's lines for the tree: sorted by bytes, as it sorts them"""
    lines = []

    def walk(rel):
        full = os.path.join(top, rel.lstrip("/"))
        lines.append("D " + (rel or "/"))
        names = sorted(os.listdir(os.fsencode(full)))
        names = [os.fsdecode(n) for n in names]
        files = []
        for n in names:
            p = os.path.join(full, n)
            if os.path.isdir(p):
                walk(rel + "/" + n)
            else:
                files.append(n)
        if len(names) > FOLD:
            fold = hashlib.sha256()
            for n in files:
                fold.update(("%s %s\n" % (n, sha(os.path.join(full, n))))
                            .encode())
            lines.append("M %s %d %s" % (rel, len(files), fold.hexdigest()))
        else:
            for n in files:
                p = os.path.join(full, n)
                lines.append("F %d %s %s/%s" % (os.path.getsize(p), sha(p),
                                                rel, n))

    walk("")
    # the serial line is read a byte to a character (latin-1): a name in
    # UTF-8 arrives as its bytes, which is what is compared
    return [l.encode("utf-8").decode("latin-1") for l in lines]


def walk_on(img, slog, label):
    """Boot on img, run fswalk, return (boot text, fswalk's lines)"""
    q = Qemu(KERNEL, INITRD, img, sata=True, append=TEXT_MODE, serial=slog)
    try:
        boot = q.wait_serial(PROMPT, timeout=90)
        out = q.command("run fswalk /", "END dirs=", timeout=600)
        out += q.wait_serial(PROMPT, timeout=30)
    finally:
        q.close()
    got = [l.strip() for l in out.splitlines()
           if l.strip()[:2] in ("D ", "F ", "M ") or
           l.strip().startswith(("END", "ERROR"))]
    return boot, got


def compare(label, want, boot, got):
    check("%s: mounted as TFS4 ('%s')" % (label, next(
          (l.strip() for l in boot.splitlines() if "TFS4" in l), "no line")),
          "[ OK ] TFS4:" in boot)
    errors = [l for l in got if l.startswith("ERROR")]
    check("%s: fswalk read everything (%s)" % (label, errors[:3]), not errors)
    files = sum(1 for l in want if l.startswith("F ")) + MANY
    dirs = sum(1 for l in want if l.startswith("D "))
    check("%s: %d directories and %d files, as the host has"
          % (label, dirs, files),
          "END dirs=%d files=%d" % (dirs, files) in got)
    missing = [l for l in want if l not in got]
    check("%s: every directory, size and SHA-256 matches hashlib's "
          "(%d of %d lines differ: %s)"
          % (label, len(missing), len(want), [m[:90] for m in missing[:3]]),
          not missing)
    for what, key in [("the 10 000 files of one directory", "M /many "),
                      ("the 70 MiB file", "/big.bin"),
                      ("the sparse file, holes as zeros", "/sparse.bin"),
                      ("the file 25 directories down", "/bottom.txt"),
                      ("the 255-byte name",
                       ("\u017c" * 4).encode("utf-8").decode("latin-1")),
                      ("a symbolic link kept in the inode", "/hello-link"),
                      ("a symbolic link kept in a block", "/deep-link")]:
        line = [l for l in want if key in l]
        check("%s: %s" % (label, what), bool(line) and line[0] in got)


def main():
    print("--- TFS4 reads what Linux's mke2fs wrote ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    if not shutil.which("mke2fs"):
        check("mke2fs (e2fsprogs) is installed - the test needs it", False)
        return summary()
    work = tempfile.mkdtemp()
    try:
        tree = os.path.join(work, "tree")
        os.mkdir(tree)
        want = build_tree(tree)
        slog = os.path.join(work, "s.log")

        for bs, size in [(4096, "200M"), (1024, "160M")]:
            img = os.path.join(work, "e2-%d.img" % bs)
            subprocess.run(["truncate", "-s", size, img], check=True)
            subprocess.run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", str(bs),
                            "-N", "16384", "-L", "ThreadOS", "-d", tree, img],
                           check=True)
            label = "%d-byte blocks" % bs
            boot, got = walk_on(img, slog, label)
            compare(label, want, boot, got)
            os.unlink(img)

        # --- whose ext2 is it -----------------------------------------
        small = os.path.join(work, "small")
        os.mkdir(small)
        with open(os.path.join(small, "hello.txt"), "wb") as f:
            f.write(b"in a partition\n")
        for kind, ttype in [("ThreadOS", OURS_TYPE), ("Linux", None)]:
            img = os.path.join(work, "gpt.img")
            make_gpt_disk(img, 800, third_type=ttype)
            subprocess.run(["mke2fs", "-q", "-F", "-t", "ext2", "-L",
                            "ThreadOS", "-E", "offset=%d" % (P3_START * 512),
                            "-d", small, img, "64M"], check=True)
            before = sha(img)
            q = Qemu(KERNEL, INITRD, img, append=TEXT_MODE, serial=slog)
            try:
                boot = q.wait_serial(PROMPT, timeout=90)
                out = q.command("cat hello.txt", PROMPT, timeout=20)
            finally:
                q.close()
            if kind == "ThreadOS":
                check("a partition of ThreadOS's type holding ext2 is "
                      "mounted, from its first sector ('%s')"
                      % next((l.strip() for l in boot.splitlines()
                              if "TFS4" in l), "no line"),
                      "[ OK ] TFS4:" in boot and
                      "from sector %d" % P3_START in boot)
                check("and its file reads", "in a partition" in out)
            else:
                check("the same ext2 in a partition of Linux's type is not "
                      "taken, though labelled ThreadOS",
                      "TFS4:" not in boot and "in a partition" not in out)
                check("and not one byte of that disk changed",
                      sha(img) == before)
            os.unlink(img)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
