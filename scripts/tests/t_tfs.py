#!/usr/bin/env python3
"""ThreadOS smoke test: the TFS file system on the disk.

A file written by a ring 3 process and a file written from the kernel's
shell are to be the same file seen by both - and both are to survive a
restart of the machine. That last part matters most here: everything
else can be faked with a cache, and a SECOND BOOT on the same disk image
cannot. After the second boot the disk image is also read on the host,
by code that knows the layout and not the kernel's code.

Everything is read from the serial port: GRUB gives the kernel a
framebuffer, so the VGA character memory says nothing.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, TfsImage

PROMPT = "64>"
RING3 = "ThreadOS 64: a file written from ring 3"   # user64/fileio64.c
TEXT = "hello-from-the-tfs-disk"


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    slog = tempfile.mktemp(suffix=".log")

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
    try:
        boot = q.wait_serial("thread64>", timeout=90)
        check("the first boot formatted the empty disk",
              "TFS: an empty disk formatted" in boot)
        q.wait_serial("[net] DHCP", timeout=30)
        time.sleep(0.5)

        # A ring 3 process writes and reads a file with syscalls
        out = q.command("run fileio64", "[fileio64] done", timeout=40)
        check("a ring 3 process wrote and read a file off the disk",
              "written and read back match byte for byte" in out
              and "[fileio64] done, exit code 0" in out)

        # The kernel's shell: write -> cat (a write and a read within
        # one boot)
        out = q.command("write greeting " + TEXT, PROMPT)
        check("writing a file from the kernel's shell",
              "write: 'greeting' written" in out)
        out = q.command("cat greeting", PROMPT)
        check("reading the written file back (cat)", TEXT in out)

        # fls shows both files - the ring 3 one too, so it is one disk
        out = q.command("fls", "Used:")
        check("fls lists both files on the disk",
              "greeting  (" in out and "zring3  (" in out)

        out = q.command("cat nofile", PROMPT)
        check("cat reports that the file is missing",
              "there is no file 'nofile'" in out)

        # A ring 3 'cat': it reads a file through a descriptor and
        # writes to descriptor 1 - the file the kernel shell wrote
        out = q.command("run ucat64 greeting", "[ucat64] done", timeout=30)
        check("a ring 3 cat reads the shell's file through an fd",
              TEXT in out.split("run ucat64 greeting", 1)[-1]
              and "[ucat64] done, exit code 0" in out)
    finally:
        q.close()

    # --- Persistence: a second boot on THE SAME disk ---
    print("--- a second boot (the same disk) ---")
    slog2 = tempfile.mktemp(suffix=".log")
    q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog2)
    try:
        boot = q2.wait_serial("thread64>", timeout=90)
        check("the second boot found the file system and did not format "
              "it again",
              "+ TFS ready" in boot and "formatted" not in boot)
        q2.wait_serial("[net] DHCP", timeout=30)
        time.sleep(0.5)

        out = q2.command("fls", "Used:")
        check("the files survived the restart (fls)",
              "greeting  (" in out and "zring3  (" in out)
        out = q2.command("cat greeting", PROMPT)
        check("the file's content survived the restart (cat)", TEXT in out)
        out = q2.command("cat zring3", PROMPT)
        check("the file written by ring 3 survived the restart", RING3 in out)

        # --- a new name, without moving a single block ---------------
        out = q2.command("run core mv greeting powitanie", "[core] done")
        check("mv ends without an error", "[core] done, exit code 0" in out)
        out = q2.command("fls", "Used:")
        check("mv gives a new name, and the old one goes",
              "powitanie  (" in out and "greeting  (" not in out)
        out = q2.command("cat powitanie", PROMPT)
        check("and the content is the same", TEXT in out)

        # The rules a rename has: the name must be free, and the source
        # must exist. A rename that overwrites is a delete nobody asked
        # for.
        out = q2.command("run core mv powitanie zring3", "[core] done")
        check("it does not allow a name that is taken",
              "mv: cannot move 'powitanie' to 'zring3'" in out
              and "[core] done, exit code 1" in out)
        out = q2.command("run core mv nosuchfile whatever", "[core] done")
        check("and it does not rename something that is not there",
              "mv: cannot move 'nosuchfile'" in out
              and "[core] done, exit code 1" in out)
        out = q2.command("fls", "Used:")
        check("after both refusals the files are as they were",
              "powitanie  (" in out and "zring3  (" in out
              and "whatever" not in out)
        out = q2.command("cat zring3", PROMPT)
        check("and the file whose name was asked for is untouched",
              RING3 in out)
    finally:
        q2.close()

    # --- The image read on the host --------------------------------
    # By TfsImage, written from the layout's description: a rename that
    # copied the blocks and left the old ones taken would show here as
    # a leak, and a name changed only in a cache as the old name
    try:
        img = TfsImage(disk)
        check("on the disk image: the renamed file holds the shell's text",
              (img.read("/powitanie") or b"").decode(errors="replace")
              .strip() == TEXT)
        check("on the disk image: the ring 3 file holds what the program "
              "wrote",
              (img.read("/zring3") or b"").decode(errors="replace")
              .strip() == RING3)
        check("on the disk image: the old name is gone",
              img.find("/greeting") is None)
        taken, past, free_held = img.leaks()
        check(f"on the disk image: every block taken belongs to a file "
              f"({len(taken)} taken but not held, {len(past)} past a "
              f"file's end, {len(free_held)} held but free)",
              not taken and not past and not free_held)
    finally:
        for f in (disk, slog, slog2):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
