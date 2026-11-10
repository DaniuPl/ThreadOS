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
  'format N' takes ONE partition: the table and the neighbours come
  out of it untouched, and after a restart the system finds its
  filesystem where it left it - which is what installing next to
  somebody else's Windows actually means

The machine stays at its text shell; everything it says is read off the
serial port, from the moment it starts.

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
from ptest import (check, summary, make_gpt_disk, gpt_intact,
                   neighbours_intact)

PROMPT = "thread64>"
# The password of the administrator a text-mode machine with no account
# database falls back on (kernel/config.c, the legacy 'password' entry)
ADMIN_PASSWORD = "thread"


def answer(q, cmd, prefix, timeout=30):
    """Types a command and hands back what arrived after it, once the
    line holding 'prefix' has come in WHOLE. The serial log fills a
    character at a time: a wait for 'format:' alone can return before
    the rest of that line - the part the check reads - is there."""
    start = len(q.serial())
    q.type_line(cmd)
    line = re.compile(re.escape(prefix) + r"[^\n]*\n")
    end = time.time() + timeout
    while time.time() < end:
        fresh = q.serial()[start:]
        if line.search(fresh):
            return fresh
        time.sleep(0.3)
    return q.serial()[start:]


def sudo(q, command, wait, timeout=30):
    """'sudo <command>' and the admin password on the line it asks for
    it on; what the command printed after the password."""
    q.command("sudo " + command, "password:", timeout=timeout)
    return q.command(ADMIN_PASSWORD, wait, timeout=timeout)


def main():
    disk = tempfile.mktemp(suffix=".img")
    make_gpt_disk(disk, 800)

    try:
        check("the test image really has a GPT", gpt_intact(disk))

        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE)
        try:
            boot = q.wait_serial(PROMPT, timeout=40)
            check("the kernel says there is something on the disk and does not touch it",
                  "there IS something on the disk" in boot)
            # The shell's own help lists 'format [N]', so the hint is
            # looked for in the words fs_init itself uses
            check("and it hints what to do if it is to be ours after all",
                  "type 'format'" in boot and "'format N'" in boot)

            # What it can read. Not the point of the group, but the
            # thing that makes the restraint useful rather than blind.
            # The PCI list comes after the disk's, so once it shows the
            # partitions have all been printed.
            said = q.command("hardware", "PCI:", timeout=30)
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
        if os.path.exists(disk):
            os.unlink(disk)

    # --- and a disk nobody loses anything by --------------------------
    blank = tempfile.mktemp(suffix=".img")
    with open(blank, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    try:
        q = Qemu(KERNEL, INITRD, blank, append=TEXT_MODE)
        try:
            boot = q.wait_serial(PROMPT, timeout=40)
            check("an empty disk is formatted as it always was",
                  "TFS: an empty disk formatted" in boot)
            check("and there is no talk of a refusal there",
                  "there IS something on the disk" not in boot)
        finally:
            q.close()
    finally:
        if os.path.exists(blank):
            os.unlink(blank)

    # --- living inside a partition, next to somebody else -------------
    # This is what installing on a laptop actually means: one partition
    # becomes ours, the table and the neighbours stay exactly as they
    # were, and after a restart the system finds its filesystem where
    # it left it.
    disk3 = tempfile.mktemp(suffix=".img")
    make_gpt_disk(disk3, 800)
    try:
        q = Qemu(KERNEL, INITRD, disk3, append=TEXT_MODE)
        try:
            q.wait_serial(PROMPT, timeout=40)
            said = sudo(q, "format 3", "Type YES")
            check("'format 3' says it will erase THAT partition, not the disk",
                  "ERASE PARTITION 3" in said and
                  "partition table stay untouched" in said)
            said = answer(q, "YES", "format:", timeout=60)
            check("and it creates a TFS on it",
                  "this partition is a ThreadOS TFS now" in said)

            out = answer(q, "write proof.txt installed-beside-windows",
                         "write:")
            check("a file goes onto the new filesystem",
                  "write: 'proof.txt' written" in out)
        finally:
            q.close()

        check("the partition table survived formatting a partition",
              gpt_intact(disk3))
        check("and the neighbouring partitions too - they still hold their data",
              neighbours_intact(disk3))

        # And the system finds it again on the next boot
        q = Qemu(KERNEL, INITRD, disk3, append=TEXT_MODE)
        try:
            boot = q.wait_serial(PROMPT, timeout=40)
            check("after the restart the kernel finds the TFS in the partition ('%s')"
                  % next((l.strip() for l in boot.splitlines()
                          if "in partition" in l), "no line"),
                  "TFS: in partition 3" in boot)
            out = q.command("cat proof.txt", "installed-beside-windows",
                            timeout=20)
            check("and the file written before the restart is there",
                  "installed-beside-windows" in out)
        finally:
            q.close()
    finally:
        if os.path.exists(disk3):
            os.unlink(disk3)

    # --- formatting on purpose ---------------------------------------
    # The disk with the table again: this time somebody asks for it, and
    # a wrong answer has to leave the table where it is.
    disk2 = tempfile.mktemp(suffix=".img")
    make_gpt_disk(disk2, 800)
    try:
        q = Qemu(KERNEL, INITRD, disk2, append=TEXT_MODE)
        try:
            q.wait_serial(PROMPT, timeout=40)

            # Erasing a disk is changing the system: a plain user is
            # not even asked the question
            said = answer(q, "format", "format:")
            check("a plain user's 'format' is refused before it asks anything",
                  "admin access required" in said and "ERASE" not in said)

            said = sudo(q, "format", "Type YES")
            check("'format' warns what it will erase ('%s')"
                  % next((l.strip() for l in said.splitlines()
                          if "ERASE" in l), "no line"),
                  "ERASE THE WHOLE DISK" in said)
            # not the word that is asked for
            said = answer(q, "yes", "format:")
            check("a wrong answer erases nothing",
                  "nothing was erased" in said)
        finally:
            q.close()
        check("and the table is still in place", gpt_intact(disk2))

        # And now the word itself
        q = Qemu(KERNEL, INITRD, disk2, append=TEXT_MODE)
        try:
            q.wait_serial(PROMPT, timeout=40)
            sudo(q, "format", "Type YES")
            said = answer(q, "YES", "format:", timeout=60)
            check("and a typed YES formats the disk",
                  "the whole disk is a ThreadOS TFS now" in said)
        finally:
            q.close()
        check("and then the GPT is gone - which is what was asked for",
              not gpt_intact(disk2))
    finally:
        if os.path.exists(disk2):
            os.unlink(disk2)

    return summary()


sys.exit(main())
