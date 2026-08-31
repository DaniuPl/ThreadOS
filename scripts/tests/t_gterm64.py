#!/usr/bin/env python3
"""ProtonOS smoke test: the terminal in a window, 64-bit kernel.

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
from ptest import check, summary, font8x8

WX, WY = 94, 54
CX, CY = WX + 1, WY + 11
PAD, ROW_H = 4, 10
COLS, ROWS = (400 - 8) // 8, (240 - 8) // 10


def read_rows(q, ox, oy, cols, rows):
    """The terminal's text, cell by cell, through the font the kernel
    drew it with."""
    w, h, at = q.screen_raw()
    font = font8x8()

    counts = {}
    for r in range(rows):
        for y in range(8):
            yy = oy + r * ROW_H + y
            for x in range(ox, ox + cols * 8, 2):
                p = at(x, yy)
                counts[p] = counts.get(p, 0) + 1
    bg = max(counts, key=counts.get)

    out = []
    for r in range(rows):
        line = ""
        for c in range(cols):
            pattern = []
            for y in range(8):
                bits = 0
                for x in range(8):
                    if at(ox + c * 8 + x, oy + r * ROW_H + y) != bg:
                        bits |= 1 << x
                pattern.append(bits)
            line += font.get(tuple(pattern), " " if not any(pattern) else "?")
        out.append(line.rstrip())
    return out


def wait_for(q, want, geom, tries=15):
    seen = []
    for _ in range(tries):
        seen = read_rows(q, **geom)
        if any(want in r for r in seen):
            return seen
        time.sleep(0.5)
    return seen


def main():
    print("--- terminal w oknie (jadro 64-bitowe) ---")

    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("POMINIETE: brak obrazu 64-bitowego (make kernel64)")
        return 0
    if shutil.which("grub-mkrescue") is None:
        print("POMINIETE: brak grub-mkrescue")
        return 0
    if shutil.which("qemu-system-x86_64") is None:
        print("POMINIETE: brak qemu-system-x86_64")
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
        shutil.copy(KERNEL64, d + "/boot/protonos64.bin")
        mod = ""
        if INITRD64 and os.path.exists(INITRD64):
            shutil.copy(INITRD64, d + "/boot/initrd64.img")
            mod = "    module2 /boot/initrd64.img initrd64\n"
        with open(d + "/boot/grub/grub.cfg", "w") as fh:
            fh.write("insmod all_video\nset gfxpayload=keep\n"
                     "set timeout=0\nset default=0\n"
                     'menuentry "ProtonOS 64" {\n'
                     "    multiboot2 /boot/protonos64.bin\n" + mod + "}\n")
        made = subprocess.run(["grub-mkrescue", "-o", iso, d],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
        check("plyta z jadrem 64-bitowym buduje sie", made)
        if not made:
            return summary()

        q = Qemu(KERNEL, INITRD, disk=disk, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=60)
            q.type_line("pulpit")
            q.wait_serial("Pulpit startuje", timeout=30)
            time.sleep(4)
            q.mouse_home()

            q.mouse_goto(24, 192)          # the 'Term' icon
            q.mouse_click()
            time.sleep(3)
            q.mouse_goto(CX + 200, CY + 150)
            q.mouse_click()                # focus
            time.sleep(1)
            q.mouse_goto(600, 400)
            time.sleep(1)

            rows = read_rows(q, **geom)
            check("terminal sie otworzyl i ma zachete",
                  any(r.startswith("/>") for r in rows))

            # --- fork + exec + a redirect built by hand ---------------
            q.type_line("uls64")
            rows = wait_for(q, "wpisow", geom)
            body = "\n".join(rows)
            check("komenda odbita razem z zacheta", "/> uls64" in body)
            check("i widac jej wyjscie - a wiec przekierowanie zrobione "
                  "z fork, dup2 i exec zadzialalo", "wpisow" in body)

            # --- a pipe, both ends closed by the parent ---------------
            q.type_line("uecho64 hej | uupper64")
            rows = wait_for(q, "HEJ", geom)
            body = "\n".join(rows)
            check("potok przepuscil tekst przez druga strone (HEJ)",
                  "HEJ" in body)
            check("a poprzednia komenda wciaz jest nad nim - to sesja",
                  "/> uls64" in body)

            # --- a program that is not there --------------------------
            q.type_line("nieistniejacy")
            rows = wait_for(q, "no such program", geom)
            check("nieznany program konczy sie komunikatem "
                  "(kod 127 od dziecka po nieudanym exec)",
                  "no such program" in "\n".join(rows))

            # --- talking to a program: the shell inside the terminal ---
            # The way in is built here, not by the kernel: pipe, fork,
            # dup2. And the way out is a shared stdout - the shell's
            # children write where the shell writes, at the end of it.
            q.type_line("ush64")
            rows = wait_for(q, "ush64$", geom)
            check("shell uzytkownika wstal w oknie terminala",
                  "ush64$" in "\n".join(rows))

            q.type_line("uecho64 czesc")
            rows = wait_for(q, "czesc", geom)
            body = "\n".join(rows)
            check("to, co wpisano, doszlo do shella i wykonalo sie",
                  "ush64$ uecho64 czesc" in body)
            cmd_at = next((i for i, r in enumerate(rows)
                           if "ush64$ uecho64 czesc" in r), -1)
            out_at = next((i for i in range(cmd_at + 1, len(rows))
                           if rows[i] == "czesc"), -1)
            check("a jego wyjscie jest POD komenda, nie nadpisane zacheta",
                  cmd_at >= 0 and out_at > cmd_at)

            q.sendkey("ctrl-d")
            time.sleep(3)
            rows = read_rows(q, **geom)
            check("Ctrl+D konczy shella takze w trybie dlugim",
                  "^D" in "\n".join(rows))

            # --- and the desktop is alive after all that --------------
            w, h, at = q.screen_raw()
            check("pulpit dziala dalej",
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
