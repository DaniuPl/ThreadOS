#!/usr/bin/env python3
"""ThreadOS smoke test: the terminal's everyday commands, on both kernels.

'ls', 'ps', 'free', 'date', 'mv' and the rest used to be the kernel
shell's own commands, out of reach of the terminal in a window, which
starts programs. They are one program now (user/core.c), and the
terminal starts it for those names. Every command is held to something
outside it:

  files made, moved and listed in the terminal are looked for with the
  kernel shell's own 'fls'

  the memory 'free' reports is what the kernel shell's 'mem' says, and
  the date is the one QEMU's clock was started at

  'ps' finds the terminal and itself; 'set' reads what the kernel holds,
  and a change from a plain user is refused in words

  a pipe between two of them counts what the first wrote

The same on the 32-bit and the 64-bit kernel: one program, built twice.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64

# The terminal opens where t_gterm finds it
CX, CY = 94 + 1, 54 + CONTENT_DY


def rows(q):
    return q.term_rows(CX, CY)


def run(q, line, want=None, tries=20):
    """Types a line into the terminal; the rows once it has answered -
    'want' shows, or without one, the next prompt under the line"""
    q.type_line(line)
    seen = []
    for _ in range(tries):
        time.sleep(0.5)
        seen = rows(q)
        if want is not None:
            if any(want in r for r in seen):
                break
            continue
        at = echo_of(seen, line)
        if at >= 0 and any(r.endswith((">", "#")) for r in seen[at + 1:]):
            break
    return seen


def echo_of(seen, line):
    """The row where the prompt echoed the line - the last such row"""
    return max((i for i, r in enumerate(seen)
                if r.endswith(("> " + line, "# " + line))), default=-1)


def after(seen, line):
    """The rows a command printed: those after its own echo, up to the
    next prompt"""
    at = echo_of(seen, line)
    out = []
    for r in seen[at + 1:] if at >= 0 else []:
        if r.endswith((">", "#")) or "> " in r or "# " in r:
            break
        out.append(r)
    return out


def shell(q, cmd, want):
    """A command for the kernel's own shell: the desktop takes the keys
    from the terminal, and the answer comes on the serial port"""
    q.click_desktop()
    time.sleep(0.5)
    out = q.command(cmd, want, timeout=15)
    q.click(CX + 200, CY + 150)        # the keys back to the terminal
    time.sleep(0.5)
    q.mouse_goto(600, 440)
    return out


def session(q, tag, bits, ram_re):
    q.mouse_home()
    q.launch_dock("gterm")                   # the "Term" tile
    time.sleep(3)
    q.click(CX + 200, CY + 150)
    q.mouse_goto(600, 440)
    time.sleep(1)
    # In the root, where the kernel shell's 'fls' and 'cat' look too
    run(q, "cd /")

    seen = run(q, "uname", "ThreadOS")
    check("%s uname says which system this is (%s)"
          % (tag, after(seen, "uname")[:1]),
          after(seen, "uname")[:1] == ["ThreadOS " + bits])

    # --- files, made here and found by the kernel ---------------------
    # (the kernel's shell starts in the profile's home: into the root)
    shell(q, "cd /", None)
    shell(q, "write /note.txt the kernel wrote this", "wrote")
    run(q, "cp note.txt copy.txt")
    run(q, "mkdir box")
    run(q, "mv copy.txt box/moved.txt")
    run(q, "touch made.txt")
    got = shell(q, "cat /box/moved.txt", "wrote this")
    check("%s cp, mkdir and mv put the kernel's file in box/moved.txt - "
          "the kernel's own 'cat' reads it there" % tag,
          "the kernel wrote this" in got)
    fls = shell(q, "fls", "made.txt")
    check("%s touch made an empty file, and the copy left the top "
          "('fls' lists made.txt, box/, not copy.txt)" % tag,
          "made.txt" in fls and "box/" in fls and "copy.txt" not in fls)
    seen = run(q, "ls box", "moved")
    check("%s 'ls' lists the directory (%s)"
          % (tag, after(seen, "ls box")[:1]),
          after(seen, "ls box")[:1] == ["moved.txt"])
    seen = run(q, "ls -l", "box/")
    out = after(seen, "ls -l")
    check("%s 'ls -l' gives sizes and marks a directory (%s)" % (tag, out),
          any(r.split() == ["dir", "box/"] for r in out) and
          any(r.split() == ["0", "made.txt"] for r in out) and
          any(r.split() in (["21", "note.txt"], ["22", "note.txt"])
              for r in out))
    run(q, "rm box/moved.txt")
    got = shell(q, "cat /box/moved.txt", "no file")
    check("%s and rm removes it, as far as the kernel is concerned"
          % tag, "there is no file" in got)

    # --- figures against the kernel's and QEMU's -----------------------
    mem = shell(q, "mem", "KiB")
    m = re.search(ram_re, mem)
    seen = run(q, "free", "KiB free")
    got = " ".join(after(seen, "free"))
    f = re.search(r"Memory: (\d+) KiB", got)
    check("%s free reports the memory the kernel's 'mem' does (%s, %s)"
          % (tag, f.group(1) if f else "?", m.group(1) if m else "?"),
          f is not None and m is not None and f.group(1) == m.group(1))
    seen = run(q, "date", RTC_DATE)
    check("%s date is the day QEMU's clock started at (%s)"
          % (tag, after(seen, "date")[:1]),
          after(seen, "date")[:1] and
          after(seen, "date")[0].startswith(RTC_DATE))

    # --- processes, and settings ----------------------------------------
    seen = run(q, "ps", "core")
    out = " ".join(after(seen, "ps"))
    check("%s ps lists the terminal and itself" % tag,
          "gterm" in out and "core" in out)
    seen = run(q, "set theme", "theme =")
    check("%s set reads a setting (%s)" % (tag, after(seen, "set theme")[:1]),
          after(seen, "set theme")[:1] == ["theme = dark"])
    # A change goes through when the person at the machine may make it
    # and is refused in words when not - and the kernel agrees either way
    seen = run(q, "set motion 0")
    out = " ".join(after(seen, "set motion 0"))
    kernel = shell(q, "set motion", "motion =")
    if "needs admin" in out:
        check("%s a plain user's change is refused, and the kernel keeps "
              "motion = 1" % tag, "motion = 1" in kernel)
    else:
        check("%s an admin's change is saved (%s), and the kernel holds "
              "motion = 0" % (tag, out),
              "motion = 0 (saved)" in out and "motion = 0" in kernel)

    # --- a pipe between two of them -------------------------------------
    seen = run(q, "echo one two three | wc", "14")
    out = after(seen, "echo one two three | wc")
    check("%s 'echo one two three | wc': 1 line, 3 words, 14 bytes (%s)"
          % (tag, out[:1]),
          out[:1] and out[0].split() == ["1", "3", "14"])


def main():
    print()
    print("--- the terminal's commands (the 32-bit kernel) ---")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=tempfile.mktemp(suffix=".log"))
        try:
            check("(32) the desktop came up", q.wait_desktop(timeout=40))
            session(q, "(32)", "i386 (protected mode)",
                    r"RAM detected: (\d+) KiB")
        finally:
            q.close()
    finally:
        os.unlink(disk)

    print("--- the terminal's commands (the 64-bit kernel) ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    work = tempfile.mkdtemp()
    iso = os.path.join(work, "t.iso")
    disk = os.path.join(work, "disk.img")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        check("(64) the disc builds", make_iso64(os.path.join(work, "iso"),
                                                 iso))
        q = Qemu(KERNEL, INITRD, cdrom=iso, disk=disk, sata=True,
                 serial=os.path.join(work, "serial.log"),
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=90)
            q.wait_serial("[net] DHCP", timeout=60)
            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=30)
            check("(64) the desktop came up", q.wait_desktop(30))
            session(q, "(64)", "x86-64 (long mode)",
                    r"Memory: (\d+) KiB RAM")
        finally:
            q.close()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
