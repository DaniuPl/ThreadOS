#!/usr/bin/env python3
"""ThreadOS smoke test: the screen's memory is write-combining (64-bit).

On the first real laptop the desktop animated in jerks: the firmware
marks the framebuffer uncached, so every store went to the graphics
card on its own, and a frame took tens of milliseconds. Under QEMU it
never showed - the "card" is RAM. The kernel now reprograms the page
attribute table so that a page entry with PWT alone means
write-combining, and maps the framebuffer with PWT.

What can be checked from outside is the page tables, through QEMU's own
monitor ('info tlb'): the framebuffer's pages carry PWT and not PCD,
and no other page carries PWT - the entry was write-through, and from
now on it means something else, so nothing else may be using it. (The
PAT MSR itself QEMU does not show; the kernel's line at boot says what
it set.)

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def main():
    print()
    print("--- the screen's memory: write-combining ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    work = tempfile.mkdtemp()
    try:
        iso = os.path.join(work, "t.iso")
        check("the disc with the 64-bit kernel builds (graphics on)",
              make_iso64(os.path.join(work, "iso"), iso, graphics=True))
        q = Qemu(KERNEL, INITRD, cdrom=iso,
                 serial=os.path.join(work, "s.log"),
                 qemu="qemu-system-x86_64")
        try:
            out = q.wait_serial("thread64>", timeout=90)
            check("the kernel says the screen memory is write-combining",
                  "screen memory write-combining" in out)
            m = re.search(r"Framebuffer: (\d+)x(\d+)", out)

            tlb = str(q.hmp("info tlb")).replace("\\r\\n", "\n")
            rows = re.findall(r"^([0-9a-f]{16}): ([0-9a-f]{16}) (\S+)$",
                              tlb, re.M)
            # QEMU's standard VGA puts the framebuffer at 0xfd000000
            fb = [r for r in rows if 0xFD000000 <= int(r[0], 16) < 0xFE000000]
            others = [r for r in rows if r not in fb]
            check(f"the framebuffer's pages are mapped ({len(fb)} entries, "
                  f"{m.group(1) + 'x' + m.group(2) if m else '?'})",
                  len(fb) > 0)
            check("with PWT and without PCD - PAT entry 1, write-combining "
                  f"({sorted(set(r[2] for r in fb))})",
                  fb and all("T" in r[2] and "C" not in r[2] for r in fb))
            stray = [r for r in others if "T" in r[2]]
            check(f"and no other page uses that entry ({len(others)} other "
                  f"entries, {len(stray)} with PWT)", others and not stray)
        finally:
            q.close()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
