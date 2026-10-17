#!/usr/bin/env python3
"""ThreadOS smoke test: the graphical desktop in the 64-bit kernel.

The compositor, the windows and the dock are the same desktop.c as in
the 32-bit kernel - it came across without a single change. The whole
difference sits in the compatibility dictionary: the desktop starts
applications by name and takes the keyboard over, and both of those have
a different name in this kernel.

We check what can be seen: the background, the bar, the dock, the
windows, the focus after a click and that the cursor moves with the
mouse.
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
    print("--- the graphical desktop (the 64-bit kernel) ---")

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

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=60)
            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=30)
            time.sleep(4)

            w, h, at = q.screen_raw()
            check(f"the desktop comes up in graphics mode ({w}x{h})",
                  w >= 640 and h >= 400)
            mid = (w // 2, h // 2)
            check("the aurora fills the screen (%s at the middle, %s expected)"
                  % (at(*mid), aurora_px(*mid, w=w, h=h)),
                  at(*mid) == aurora_px(*mid, w=w, h=h))
            check("the menu bar is drawn - the aurora through glass",
                  near(at(w // 2, 4), bar_glass(True, w, h)[4][w // 2], 1))
            tile = dock_tile(0, w=w, h=h)
            check("the dock is drawn: the first program's tile (%s)"
                  % (at(*tile),), min(at(*tile)) > 200)

            # The kernel's windows: two, one on top of the other - their
            # bodies where each stands
            body = theme("dark")["win_bg"]
            check("the 'Terminal' window is drawn",
                  near(at(130, 120), body, 3))
            check("the 'Files' window is drawn",
                  near(at(240, 140), body, 3))

            # The mouse: the cursor is to go where we move it. We check
            # that through a change of pixels at the destination.
            q.mouse_goto(400, 300)
            time.sleep(1.5)
            _, _, at2 = q.screen_raw()
            differ = sum(1 for dy in range(12) for dx in range(12)
                        if at2(400 + dx, 300 + dy) != at(400 + dx, 300 + dy))
            check(f"the pointer follows the mouse ({differ} pixels changed)",
                  differ > 8)

            # A click on the second window's title bar brings it to the
            # top - and that can be seen from it covering the first
            q.mouse_goto(240, 100)
            q.mouse_button(True)
            time.sleep(0.3)
            q.mouse_button(False)
            time.sleep(1.5)
            _, _, at3 = q.screen_raw()
            check("a click on the title bar gives that window the focus",
                  q.has_focus(180, 95))
            check("and does not upset the compositor",
                  at3(*mid) == aurora_px(*mid, w=w, h=h))

            # A key that no window wants goes to the kernel's shell -
            # the same as in protected mode. Long mode had nowhere to
            # hand those keys back to, so a desktop full of windows was
            # a screen nobody could type at.
            q.mouse_goto(560, 300)
            q.mouse_button(True)          # the desktop takes the focus
            time.sleep(0.3)
            q.mouse_button(False)
            time.sleep(1)
            seen = len(q.serial())
            q.type_line("uptime")
            time.sleep(2)
            out = q.serial()[seen:]
            check("a key no window wants reaches the "
                  "kernel's shell", "Uptime:" in out)
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
