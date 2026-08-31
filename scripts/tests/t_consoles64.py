#!/usr/bin/env python3
"""ProtonOS smoke test: virtual consoles in the 64-bit kernel.

The hardware has one screen, and the system holds four. Alt+F1..F4
switches the picture, but every console has its own content, its own
shell and its own writers: which console a program writes on is decided
by the TASK, not by what we happen to be looking at. That is why a
program started on the second console keeps writing on it while we look
at the first - and that is this test's real assertion, stronger than the
picture switching itself.

The whole console buffer is in kernel/vga.c, shared with the 32-bit
kernel. In long mode task ownership (task_vc) and a separate key queue
per console were added to it.

We read the screen from the framebuffer rather than from the character
memory at 0xB8000: GRUB, seeing a request for a video mode in the
kernel's header, gives graphics anyway, so the console draws itself in
an 8x8 font. So the reading is the one a human gets - from what can
really be seen.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def main():
    print("--- konsole wirtualne Alt+F1..F4 (jadro 64-bitowe) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    try:
        check("plyta z jadrem 64-bitowym buduje sie", make_iso64(d, iso))
        if not os.path.exists(iso):
            return summary()

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=60)

            # The first console: something that will stay on it for a while
            q.type_line("clear")
            q.type_line("uptime")
            time.sleep(1.0)
            first_one = q.wait_fbcon("Uptime:", timeout=10)
            check("pierwsza konsola dziala jak dotad",
                  "Uptime:" in first_one)

            # Alt+F2: the second console has its own shell and is empty
            q.sendkey("alt-f2")
            time.sleep(0.8)
            second_one = q.screen_fbcon()
            check("Alt+F2 pokazuje druga konsole z wlasnym promptem",
                  "konsola 2" in second_one)
            check("tresc pierwszej konsoli nie przecieka na druga",
                  "Uptime:" not in second_one)

            # A program started on the second console writes on it
            q.type_line("run spin64")
            view = q.wait_fbcon("[spin64]", timeout=20)
            check("program uruchomiony na drugiej konsoli pisze na niej",
                  "[spin64]" in view)

            # Back to the first: its content untouched, and the program
            # from the second still writes on its own, not here
            q.sendkey("alt-f1")
            time.sleep(1.5)
            returned = q.screen_fbcon()
            check("pierwsza konsola nietknieta po powrocie",
                  "Uptime:" in returned)
            check("program z drugiej konsoli nie pisze po pierwszej",
                  "[spin64]" not in returned)

            # Ctrl+C concerns the console it falls on: here there is
            # nothing to interrupt, and the program on the second is to
            # keep running
            q.sendkey("ctrl-c")
            time.sleep(1.0)
            q.sendkey("alt-f2")
            time.sleep(1.0)
            again = q.screen_fbcon()
            check("program na drugiej konsoli przezyl Ctrl+C na pierwszej",
                  "[spin64]" in again)

            # Ctrl+C on its own console ends the program. The proof is
            # double: the shell reports the child's end, and the screen
            # stops changing - spin64 writes every half second, so alive
            # it would leave a trace.
            q.sendkey("ctrl-c")
            time.sleep(2.0)
            po = q.screen_fbcon()
            check("Ctrl+C na wlasnej konsoli przerywa program",
                  "koniec, kod wyjscia" in po)
            time.sleep(2.0)
            check("przerwany program przestal pisac",
                  q.screen_fbcon() == po)
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
