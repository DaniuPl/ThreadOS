#!/usr/bin/env python3
"""ThreadOS smoke test: the loading screen of a boot to the desktop.

A boot that ends at the desktop shows a loading screen instead of the
kernel's text console: a dark background over the whole framebuffer
the loader set, the name, a line saying what is being done and a
progress bar. The kernel's log does not stop - it goes to the serial
port alone, where whoever needs it can still read it.

What is checked is what can be seen, while it can be seen: the screen
is read from the moment QEMU starts, over and over for as long as the
splash is up, until the desktop takes its place. The splash covers the
mode GRUB left - whatever size that is, which the kernel reports on the
serial port - so the expectations are worked out from that size, the
way kernel/bootsplash.c works out where to draw.

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
from ptest import check, summary, no64

# kernel/bootsplash.c's colours
SPLASH_BG = (0x08, 0x11, 0x1F)
SPLASH_TEXT = (0xE8, 0xF0, 0xFA)
SPLASH_ACCENT = (0x57, 0xB8, 0xFF)


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    log = tempfile.mktemp(suffix=".log")
    q = Qemu(KERNEL, INITRD, serial=log)
    try:
        def splash_up(s):
            """Every corner of the screen in the splash's background"""
            w, h, at = s
            return w >= 320 and all(
                at(x, y) == SPLASH_BG
                for x, y in ((2, 2), (w - 3, 2), (2, h - 3), (w - 3, h - 3)))

        def shot():
            try:
                return q.screen_raw()
            except Exception:
                return None

        first = None
        end = time.time() + 60
        while time.time() < end and first is None:
            s = shot()
            if s and splash_up(s):
                first = s
            time.sleep(0.05)
        check("the splash covers the whole screen, corner to corner",
              first is not None)
        if first is None:
            return summary()
        w, h, _ = first

        # Where bootsplash.c puts things, for this size
        scale = 3 if w >= 640 else 2
        cy = h // 2
        tx, ty = (w - 8 * 8 * scale) // 2, cy - 58 * scale // 2
        bar_w = min(w - 64, 480)
        bx, by = (w - bar_w) // 2, cy + 38
        near_end = bx + bar_w * 2 // 5      # filled from 40 per cent on
        # The console draws its 80x25 cells of 8x8 in the middle of the
        # screen; above the name nothing of the splash's is there
        top = (h - 25 * 8) // 2
        band = [(x, y) for y in range(top, ty - 2)
                for x in range((w - 80 * 8) // 2, (w + 80 * 8) // 2)]

        # Watch it for as long as it is up
        title_px, started, bar_empty, bar_full, dirty, shots = 0, 0, 0, 0, 0, 0
        s = first
        end = time.time() + 60
        while time.time() < end:
            if s and (s[0], s[1]) == (w, h) and splash_up(s):
                at = s[2]
                shots += 1
                title_px = max(title_px, sum(
                    at(x, y) == SPLASH_TEXT
                    for y in range(ty, ty + 8 * scale)
                    for x in range(tx, tx + 8 * 8 * scale)))
                started += at(bx + 1, by + 2) == SPLASH_ACCENT
                if at(near_end, by + 2) == SPLASH_ACCENT:
                    bar_full += bar_empty > 0
                else:
                    bar_empty += 1
                dirty += any(at(x, y) != SPLASH_BG for x, y in band)
            elif s and s[0] >= 640 and not splash_up(s):
                break                      # the desktop is there
            s = shot()
            time.sleep(0.05)

        serial = q.wait_serial("[boot] visual loading screen active",
                               timeout=30)
        check("the kernel reports the graphical boot path",
              "[boot] visual loading screen active" in serial)
        m = re.search(r"Framebuffer: (\d+)x(\d+)", serial)
        check("the splash fills the framebuffer the loader set (%dx%d, "
              "the kernel says %s)" % (w, h, m.group(0) if m else "nothing"),
              m is not None and (int(m.group(1)), int(m.group(2))) == (w, h))
        check("the name is drawn in the middle (%d of its pixels)"
              % title_px, title_px > 200)
        check("the progress bar begins to fill", started > 0)
        check("and fills further as the boot goes on",
              bar_full > 0)
        check("nothing of the console is drawn over the splash - no log "
              "line, no uptime in its corner (%d of %d looks)"
              % (dirty, shots), shots > 0 and dirty == 0)

        # The log goes on, on the serial port only, past the moment the
        # splash came up
        after = q.wait_serial("The 64-bit system is ready", timeout=60)
        check("the kernel's log carries on on the serial port",
              after.find("initrd:") >
              after.find("[boot] visual loading screen active"))

        check("and the desktop takes the splash's place",
              q.wait_desktop(timeout=60))
    finally:
        q.close()
        if os.path.exists(log):
            os.unlink(log)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
