#!/usr/bin/env python3
"""ThreadOS smoke test: the terminal in a window, 64-bit kernel.

The terminal is one program compiled twice - the ring of lines, the
editing, the history and the drawing are the same code on both sides.
One thing is not: how a command is started and its output caught. In
protected mode the kernel has a call that spawns a program with its
output redirected, and another that runs a whole pipeline. In long mode
there is no such call, so the terminal builds it itself out of fork,
dup2, exec and pipe - the way a shell does.

That is the part this group is for. Everything here goes through the
64-bit path: a program started and waited for, its output read back
from a file, a pipe with both ends handed to two children and closed by
the parent, and a program that does not exist saying so - which in this
kernel can only be told from the child's exit code, because a failed
exec has no other way home.

The text is read off the screen through the kernel's own font, the same
as in the 32-bit group.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

WX, WY = 94, 54
CX, CY = WX + 1, WY + CONTENT_DY
PAD, ROW_H = TERM_PAD, TERM_ROW_H
COLS, ROWS = TERM_COLS, TERM_ROWS


def read_rows(q, ox, oy, cols, rows):
    """The terminal's text, read off the screen in the lettering it is
    drawn in: a grid of cells, each read on its own (ptest.read_cells)"""
    return q.read_cells(ox, oy, cols, rows, cell=TERM_CELL, row_h=ROW_H,
                        base=TERM_BASE, px=TERM_PX)


def wait_for(q, want, geom, tries=15):
    seen = []
    for _ in range(tries):
        seen = read_rows(q, **geom)
        if any(want in r for r in seen):
            return seen
        time.sleep(0.5)
    return seen


def main():
    print("--- the terminal in a window (the 64-bit kernel) ---")

    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("SKIPPED: no 64-bit image (make kernel64)")
        return 0
    if shutil.which("grub-mkrescue") is None:
        print("SKIPPED: no grub-mkrescue")
        return 0
    if shutil.which("qemu-system-x86_64") is None:
        print("SKIPPED: no qemu-system-x86_64")
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    geom = dict(ox=CX + PAD, oy=CY + PAD, cols=COLS, rows=ROWS)

    try:
        os.makedirs(d + "/boot/grub")
        shutil.copy(KERNEL64, d + "/boot/threados64.bin")
        mod = ""
        if INITRD64 and os.path.exists(INITRD64):
            shutil.copy(INITRD64, d + "/boot/initrd64.img")
            mod = "    module2 /boot/initrd64.img initrd64\n"
        with open(d + "/boot/grub/grub.cfg", "w") as fh:
            fh.write("insmod all_video\nset gfxpayload=keep\n"
                     "set timeout=0\nset default=0\n"
                     'menuentry "ThreadOS 64" {\n'
                     "    multiboot2 /boot/threados64.bin\n" + mod + "}\n")
        made = subprocess.run(["grub-mkrescue", "-o", iso, d],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
        check("the disc with the 64-bit kernel builds", made)
        if not made:
            return summary()

        q = Qemu(KERNEL, INITRD, disk=disk, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=60)
            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=30)
            q.wait_desktop(40)
            time.sleep(4)
            q.mouse_home()

            q.launch_dock(4)                # the "Term" tile
            time.sleep(3)
            q.mouse_goto(CX + 200, CY + 150)
            q.mouse_click()                # focus
            time.sleep(1)
            q.mouse_goto(600, 400)
            time.sleep(1)

            rows = read_rows(q, **geom)
            check("the terminal opened and has a prompt",
                  any(r.startswith("/>") for r in rows))

            # --- hardware inventory from the desktop terminal --------
            q.type_line("lspci")
            rows = wait_for(q, "class", geom)
            check("lspci runs from the terminal and prints PCI class IDs",
                  any("class " in r and ":" in r for r in rows))
            q.type_line("ifconfig")
            rows = wait_for(q, "Network configuration", geom)
            check("ifconfig runs from the desktop terminal",
                  "Network configuration" in "\n".join(rows))

            # --- fork + exec + a redirect built by hand ---------------
            q.type_line("uls64")
            rows = wait_for(q, "entries", geom)
            body = "\n".join(rows)
            check("the command was echoed together with the prompt", "/> uls64" in body)
            check("and its output shows - so the redirect was done "
                  "with fork, dup2 and exec worked", "entries" in body)

            # --- a pipe, both ends closed by the parent ---------------
            q.type_line("uecho64 hej | uupper64")
            rows = wait_for(q, "HEJ", geom)
            body = "\n".join(rows)
            check("the pipe passed the text through the other side (HEJ)",
                  "HEJ" in body)
            check("and the previous command is still above it - a session",
                  "/> uls64" in body)

            # --- a program that is not there --------------------------
            q.type_line("nosuchprogram")
            rows = wait_for(q, "no such program", geom)
            check("an unknown program ends with a message "
                  "(code 127 from the child after a failed exec)",
                  "no such program" in "\n".join(rows))

            # --- talking to a program: the shell inside the terminal ---
            # The way in is built here, not by the kernel: pipe, fork,
            # dup2. And the way out is a shared stdout - the shell's
            # children write where the shell writes, at the end of it.
            q.type_line("ush64")
            rows = wait_for(q, "ush64$", geom)
            check("the user's shell came up in the terminal window",
                  "ush64$" in "\n".join(rows))

            q.type_line("uecho64 hello")
            rows = wait_for(q, "hello", geom)
            body = "\n".join(rows)
            check("what was typed reached the shell and ran",
                  "ush64$ uecho64 hello" in body)
            cmd_at = next((i for i, r in enumerate(rows)
                           if "ush64$ uecho64 hello" in r), -1)
            out_at = next((i for i in range(cmd_at + 1, len(rows))
                           if rows[i] == "hello"), -1)
            check("and its output is UNDER the command, not overwritten by the prompt",
                  cmd_at >= 0 and out_at > cmd_at)

            q.sendkey("ctrl-d")
            time.sleep(3)
            rows = read_rows(q, **geom)
            check("Ctrl+D ends the shell in long mode too",
                  "^D" in "\n".join(rows))

            # --- and the desktop is alive after all that --------------
            w, h, at = q.screen_raw()
            check("the desktop is still running",
                  at(600, 500)[1] > 20 or at(600, 500)[2] > 20)
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, disk):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
