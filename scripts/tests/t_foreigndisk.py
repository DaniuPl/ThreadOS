#!/usr/bin/env python3
"""ThreadOS smoke test: a disk that is not ours is not ours to take.

Until this group existed, fs_init() did something that was harmless for
as long as every disk this system ever saw was an empty file made by a
test, and catastrophic the first time it met a real one: if the first
block did not say TFS3, it FORMATTED the disk. No question, no look at
what was there.

A laptop's disk does not say TFS3 either. Booting this system from a
stick would have thrown away the partition table, the Windows and
everything else, in the first second, without asking.

So the assertions are about restraint, and about being able to say what
one is being restrained from:

  a disk with a partition table on it comes out of a boot with that
  table still on it - read back from the host, byte for byte, not
  taken on the system's word
  and the machine says why it left it alone
  it can READ what is there: three partitions, their kinds, their
  sizes and the names they carry
  a genuinely blank disk is still formatted, because that is a disk
  nobody loses anything by
  and 'format' does it on purpose - but only for an admin, and only
  for somebody who types the whole word
  'formatuj N' takes ONE partition: the table and the neighbours come
  out of it untouched, and after a restart the system finds its
  filesystem where it left it - which is what installing next to
  somebody else's Windows actually means

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, make_gpt_disk, gpt_intact,
                   neighbours_intact)

PROMPT = "thread64>"


def boot(disk, slog):
    """The text shell on this disk, and what the kernel said on the way
    to it"""
    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
    return q, q.wait_serial(PROMPT, timeout=60)


def sudo(q, command, until, timeout=30):
    """'sudo <command>' and the admin password on the line the shell
    asks for it: formatting changes the machine, and the shell starts
    as nobody in particular. What came after the password, up to
    'until'."""
    q.command("sudo " + command, "password:", timeout=20)
    return q.command("thread", until, timeout=timeout)


def main():
    disk = tempfile.mktemp(suffix=".img")
    slog = tempfile.mktemp(suffix=".log")
    make_gpt_disk(disk, 800)

    try:
        check("the test image really has a GPT", gpt_intact(disk))

        q, said = boot(disk, slog)
        try:
            check("the kernel says there is something on the disk and does not touch it",
                  "there IS something on the disk" in said)
            check("and it hints what to do if it is to be ours after all",
                  "type 'format'" in said)

            # What it can read. Not the point of the group, but the
            # thing that makes the restraint useful rather than blind.
            said = q.command("hardware", PROMPT, timeout=30)
            check("it reads the GPT and the number of partitions ('%s')"
                  % next((l.strip() for l in said.splitlines()
                          if "table," in l), "no line"),
                  "GPT table" in said and "partitions: 3" in said)
            check("it names the EFI partition and sees its label",
                  "EFI (ESP)" in said and "EFI system partition" in said)
            check("and it recognises the data and the Linux partitions",
                  "Windows" in said and "linux-root" in said)
        finally:
            q.close()

        # The assertion the whole group exists for: read the image back
        # from the host, and the table has to still be in it.
        check("after the whole boot the partition table is UNTOUCHED",
              gpt_intact(disk))
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    # --- and a disk nobody loses anything by --------------------------
    blank = tempfile.mktemp(suffix=".img")
    slog2 = tempfile.mktemp(suffix=".log")
    with open(blank, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    try:
        q, said = boot(blank, slog2)
        try:
            check("an empty disk is formatted as it always was",
                  "an empty disk formatted" in said)
            check("and there is no talk of a refusal there",
                  "there IS something on the disk" not in said)
        finally:
            q.close()
    finally:
        for f in (blank, slog2):
            if os.path.exists(f):
                os.unlink(f)

    # --- living inside a partition, next to somebody else -------------
    # This is what installing on a laptop actually means: one partition
    # becomes ours, the table and the neighbours stay exactly as they
    # were, and after a restart the system finds its filesystem where
    # it left it.
    disk3 = tempfile.mktemp(suffix=".img")
    slog4 = tempfile.mktemp(suffix=".log")
    make_gpt_disk(disk3, 800)
    try:
        q, _ = boot(disk3, slog4)
        try:
            said = sudo(q, "format 3", "Type YES")
            check("'format 3' says it will erase THAT partition, not the disk",
                  "ERASE PARTITION 3" in said and
                  "partition table stay untouched" in said)
            said = q.command("YES", "format:", timeout=30)
            check("and it creates a TFS on it",
                  "this partition is a ThreadOS TFS now" in said)

            said = q.command("write proof.txt installed-beside-windows",
                             "write:", timeout=20)
            check("and a file goes onto it", "written" in said)
        finally:
            q.close()

        check("the partition table survived formatting a partition",
              gpt_intact(disk3))
        check("and the neighbouring partitions too - they still hold their data",
              neighbours_intact(disk3))

        # And the system finds it again on the next boot
        os.unlink(slog4)
        q, said = boot(disk3, slog4)
        try:
            check("after the restart the kernel finds the TFS in the partition ('%s')"
                  % next((l.strip() for l in said.splitlines()
                          if "in partition" in l), "no line"),
                  "TFS: in partition 3" in said)
            out = q.command("cat proof.txt", "installed", timeout=20)
            check("and the file written before the restart is there",
                  "installed-beside-windows" in out)
        finally:
            q.close()
    finally:
        for f in (disk3, slog4):
            if os.path.exists(f):
                os.unlink(f)

    # --- formatting on purpose ---------------------------------------
    # The disk with the table again: this time somebody asks for it, and
    # a wrong answer has to leave the table where it is.
    disk2 = tempfile.mktemp(suffix=".img")
    slog3 = tempfile.mktemp(suffix=".log")
    make_gpt_disk(disk2, 800)
    try:
        q, _ = boot(disk2, slog3)
        try:
            out = q.command("format", "format:", timeout=20)
            check("a plain user is not let near it",
                  "admin access required" in out)

            said = sudo(q, "format", "Type YES")
            # not the word that is asked for
            said += q.command("yes", "format:", timeout=30)
            check("'format' warns what it will erase ('%s')"
                  % next((l.strip() for l in said.splitlines()
                          if "ERASE" in l), "no line"),
                  "ERASE THE WHOLE DISK" in said)
            check("a wrong answer erases nothing",
                  "nothing was erased" in said)
        finally:
            q.close()
        check("and the table is still in place", gpt_intact(disk2))

        # And now the word itself
        os.unlink(slog3)
        q, _ = boot(disk2, slog3)
        try:
            sudo(q, "format", "Type YES")
            said = q.command("YES", "format:", timeout=30)
            check("and a typed YES formats the disk",
                  "the whole disk is a ThreadOS TFS now" in said)
        finally:
            q.close()
        check("and then the GPT is gone - which is what was asked for",
              not gpt_intact(disk2))
    finally:
        for f in (disk2, slog3):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
