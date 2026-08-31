#!/usr/bin/env python3
"""ProtonOS smoke test: the kernel shell's commands.

The kernel's shell is the only way into the system until ush starts in
ring 3 - and the first thing to stop working when something breaks
deeper down. We check that it prints what it should and that the numbers
it shows agree with reality.

The strongest assertion here is the one about managed memory: the
identity mapping is to reach all the way to the top of the detected RAM,
not to the first 64 MiB.

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
from ptest import check, summary


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    slog = tempfile.mktemp(suffix=".log")

    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
    try:
        q.wait_for("ProtonOS shell")
        # The DHCP message falls into the middle of a line being typed, so
        # we wait for it before we start typing
        q.wait_for("[siec] DHCP", timeout=20)
        time.sleep(0.5)

        q.type_line("help")
        screen = q.screen()
        check("komenda 'help' wypisuje liste komend",
              "Dostepne komendy" in screen and "reboot" in screen)

        q.type_line("mem")
        screen = q.screen()
        check("komenda 'mem' pokazuje RAM i ramki",
              "RAM wykryty" in screen and "ramek 4 KiB" in screen)
        check("komenda 'mem' pokazuje sterte", "sterta jadra" in screen)
        # All the detected memory is under management, not only the first
        # 64 MiB: the identity mapping stretches all the way to the top of
        # the RAM.
        mm = re.search(r"zarzadzane (\d+) MiB", screen)
        ram = re.search(r"RAM wykryty:\s+(\d+) KiB", screen)
        check("zarzadzana pamiec siega szczytu wykrytego RAM-u"
              + (f" ({mm.group(1)} MiB z {int(ram.group(1)) // 1024} MiB)"
                 if mm and ram else ""),
              mm is not None and ram is not None
              and int(mm.group(1)) >= int(ram.group(1)) // 1024 - 2)

        q.type_line("uptime")
        check("komenda 'uptime' dziala", "tyknac timera" in q.screen())

        q.type_line("ps")
        screen = q.screen()
        check("komenda 'ps' pokazuje watki",
              "main" in screen and "demo-A" in screen
              and "demo-B" in screen)
        check("shell jest wlasnym watkiem (nie kontekst przerwania)",
              "shell (biezacy)" in screen)
        check("zombie po watku C zostal sprzatniety",
              "demo-C" not in screen)
        check("ps pokazuje rodzica kazdego watku",
              "RODZIC" in screen
              and re.search(r"\n  0   -", screen) is not None)

        # initrd: 'ls' shows the packed programs. There are more files than
        # fit on the screen, so the early ones scroll away - and the more
        # programs are added, the more of them scroll. So we ask about the
        # SHAPE of the list rather than about particular names: every line
        # is a name and a non-zero size, there are a dozen or so lines, and
        # among them familiar programs.
        q.type_line("ls")
        screen = q.screen()
        entries = re.findall(r"(\w+)\s+\((\d+) B\)", screen)
        known = {"forktest", "ptree", "sigtest", "lazymem", "swaptest",
                 "mkmap", "mmapfile", "deep", "mmapzap", "dynlib"}
        check("komenda 'ls' pokazuje pliki initrd (%d wpisow, %d znanych)"
              % (len(entries), len({n for n, _ in entries} & known)),
              len(entries) >= 15
              and all(int(size) > 0 for _, size in entries)
              and len({n for n, _ in entries} & known) >= 3)

        q.type_line("run brak")
        check("run zglasza brak pliku - i szuka go w obu miejscach",
              "nie ma 'brak' ani w initrd, ani na dysku" in q.screen())
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
