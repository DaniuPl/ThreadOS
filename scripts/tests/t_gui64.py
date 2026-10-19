#!/usr/bin/env python3
"""ThreadOS smoke test: graphical applications in ring 3 (64-bit kernel).

The desktop has so far worked with itself alone - the compositor sat in
the kernel, and the icons pointed at programs that did not exist in long
mode. Now an icon starts a real process from the initrd, which draws in
a window of its own through system calls and gets events back.

We check that whole path the way a user sees it: clicking an icon is to
give a window with a palette drawn in it, clicking in the window is to
leave a marker in it (that is, the event reached the process and came
back as a drawing), and keys are to reach the window with focus.

The application windows follow in turn behind the two kernel windows:
the first stands at (94,54), each next one twelve pixels lower and to
the right. The content area starts a pixel to the right and a title bar
lower. All the screen coordinates below come from that one rule.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

# The compositor places the windows in a cascade: slot i at
# (70+12i, 30+12i), and the content a pixel to the right and a title bar
# (10 px) lower. The first two slots are taken by the kernel's windows,
# so the applications start from the third.
def body_text(slot):
    return 70 + 12 * slot + 1, 30 + 12 * slot + CONTENT_DY


BODY_X, BODY_Y = body_text(2)  # the top left corner of the first application's content

RED = (255, 0, 0)
GREEN = (0, 255, 0)
BLUE = (0, 0, 255)
WHITE = (255, 255, 255)
BLACK = (0, 0, 0)


def scr(cx, cy):
    """The screen coordinates of the point (cx,cy) of a window's content
    area."""
    return BODY_X + cx, BODY_Y + cy


def main():
    print("--- a graphical application in ring 3 (the 64-bit kernel) ---")

    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("SKIPPED: no 64-bit image (make kernel64)")
        return 0
    if INITRD64 is None or not os.path.exists(INITRD64):
        print("SKIPPED: no 64-bit initrd")
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
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        os.makedirs(d + "/boot/grub")
        shutil.copy(KERNEL64, d + "/boot/threados64.bin")
        shutil.copy(INITRD64, d + "/boot/initrd64.img")
        with open(d + "/boot/grub/grub.cfg", "w") as fh:
            fh.write("insmod all_video\nset gfxpayload=keep\n"
                     "set timeout=0\nset default=0\n"
                     'menuentry "ThreadOS 64" {\n'
                     "    multiboot2 /boot/threados64.bin\n"
                     "    module2 /boot/initrd64.img initrd64\n}\n")
        made = subprocess.run(["grub-mkrescue", "-o", iso, d],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
        check("the disc with the 64-bit kernel builds", made)
        if not made:
            return summary()

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=60)
            # A file on the disk for the file manager - once the desktop
            # starts the keyboard belongs to it, so it is now or never
            q.command("write note hello-from-the-disk", "write:")
            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=30)
            check("the desktop comes up in graphics mode", q.wait_desktop(30))

            # The mouse is relative: first push the cursor into the
            # corner, so as to know where it is at all.
            q.mouse_home()
            q.launch_dock(0)                # the "App" tile
            q.mouse_goto(400, 300)        # move the cursor away from the window

            # The application draws a palette of three squares at the top
            # of the window. Each from a different system call, so each
            # separately.
            cx, cy = scr(10, 10)
            check("a click on a tile starts a program from the initrd (a red square)",
                  q.wait_pixel(cx, cy, RED, timeout=20))
            cx, cy = scr(38, 10)
            check("the program draws a second square (green)",
                  q.wait_pixel(cx, cy, GREEN, timeout=5))
            cx, cy = scr(66, 10)
            check("the program draws a third square (blue)",
                  q.wait_pixel(cx, cy, BLUE, timeout=5))

            # The caption 'klikniec:' drawn by win_text - DARK text on
            # the window's light grey, which is the rule everywhere in
            # this system; white on that grey was drawn and could not
            # be read.
            # (looked at until it is there: a window grows into place
            # as it opens, and on a busy host that takes longer)
            tx, ty = scr(6, 24)
            for _ in range(10):
                _, _, at = q.screen_raw()
                whites = sum(1 for dy in range(8) for dx in range(72)
                             if at(tx + dx, ty + dy) == BLACK)
                if whites > 20:
                    break
                time.sleep(0.5)
            check(f"the program writes text in its window ({whites} dark pixels)",
                  whites > 20)

            # The click counter before the click - we remember the cell
            # with the digit, so that there is something to compare with
            lx, ly = scr(80, 24)

            def counter_cell(fun):
                return [fun(lx + dx, ly + dy)
                        for dy in range(8) for dx in range(8)]

            counter_before = counter_cell(at)

            # An event the other way: a click in the content area is to
            # reach the process, which answers with a marker in the
            # accent colour at that place. The tolerance has to be
            # narrow here: the window's grey background (227,227,227)
            # fits inside the default one and would pass a test in
            # which nothing happens.
            kx, ky = scr(20, 50)
            q.mouse_goto(kx, ky)
            q.mouse_click()
            q.mouse_goto(400, 300)
            check("a click on the window comes back to the program as an event (a mark)",
                  q.wait_pixel(kx, ky, BLUE, tol=8, timeout=10))

            # The click counter stops being zero - the program counted the
            # event rather than merely redrawing the window
            _, _, at2 = q.screen_raw()
            check("the click counter in the window grows",
                  counter_cell(at2) != counter_before)

            # The keyboard: the window has focus after the click, so the
            # typed characters are to appear in the 'txt:' field.
            px, py = scr(40, 38)

            def field_text(fun):
                return [fun(px + dx, py + dy)
                        for dy in range(8) for dx in range(40)]

            before = field_text(at2)
            q.sendkey("o")
            q.sendkey("k")
            time.sleep(1.5)
            _, _, at3 = q.screen_raw()
            after = field_text(at3)
            check("the keys reach the window with the focus", before != after)

            # --- The clock: a second application, its own window ---
            q.mouse_home()
            q.launch_dock(1)                # the "Clock" tile
            q.mouse_goto(400, 300)

            # The clock writes the time in the lettering of the system,
            # and the time is read back the same way
            zx, zy = body_text(3)
            first = ""
            end = time.time() + 20
            while time.time() < end:
                first = q.gclock_time(zx, zy)
                if len(first) == 8 and first.count(":") == 2:
                    break
                time.sleep(0.5)
            check("the 'Clock' tile starts a second application in a window "
                  "of its own ('%s')" % first, first.count(":") == 2)

            # The clock is to run: the time changes in the window of its own
            # accord
            time.sleep(2.5)
            second = q.gclock_time(zx, zy)
            check("the clock in the window runs ('%s' -> '%s')"
                  % (first, second),
                  second.count(":") == 2 and second != first)

            # --- Files: a third application, it reads the disk ---
            q.mouse_home()
            q.launch_dock(2)                # the "Files" tile
            q.mouse_goto(400, 300)

            px2, py2 = body_text(4)
            # The heading says which directory is shown
            head = ""
            end = time.time() + 20
            while time.time() < end:
                head = q.gfiles_heading(px2, py2)
                if head.startswith("/"):
                    break
                time.sleep(0.5)
            check("the 'Files' tile starts the file manager (its heading: "
                  "'%s')" % head, head.startswith("/"))

            # A click chooses a file: the row takes the accent colour,
            # which is this desktop's way of saying "this one". That is
            # the whole path being checked - a mouse event reaching a
            # ring 3 program and coming back as a drawing.
            rows = q.gfiles_rows(px2, py2)
            picked = False
            if rows:
                q.mouse_home()
                q.mouse_goto(px2 + GFILES_ROW_X, py2 + gfiles_row_y(0))
                q.mouse_click()
                q.mouse_goto(400, 300)
                for _ in range(10):
                    time.sleep(0.5)
                    rows = q.gfiles_rows(px2, py2)
                    if rows and rows[0][2]:
                        picked = True
                        break
            check("a click on a file name picks it in the manager (%s)"
                  % ", ".join(r[0] for r in rows), picked)

            # The compositor survived all this: the desktop's background is still
# there.
            w, h, at4 = q.screen_raw()
            check("the compositor still draws the desktop",
                  at4(w - 20, h // 2) == aurora_px(w - 20, h // 2, w, h))

            # --- Settings: the 64-bit-only Updates section ---------------
            q.mouse_home()
            q.launch_dock(6)                # the "Settings" tile
            q.mouse_goto(400, 300)
            swx, swy = cascade(5, GSET_W + 2, GSET_H + TITLE_H + 2)
            scx, scy = swx + 1, swy + CONTENT_DY
            head = ""
            end = time.time() + 15
            while time.time() < end:
                head = q.gset_heading(scx, scy)
                if head == "General":
                    break
                time.sleep(0.5)
            check("the Settings tile opens on General ('%s')" % head,
                  head == "General")
            updates_section = len(GSET_SECTIONS64) - 1
            side = q.gset_side(scx, scy, updates_section)
            check("64-bit Settings has an Updates section ('%s')" % side,
                  side == "Updates")
            q.click(scx + GSET_SIDE_W // 2,
                    scy + GSET_SIDE_Y0 + updates_section * GSET_SIDE_H +
                    GSET_SIDE_H // 2)
            q.mouse_goto(400, 300)
            head = ""
            end = time.time() + 15
            while time.time() < end:
                head = q.gset_heading(scx, scy)
                if head == "Updates":
                    break
                time.sleep(0.5)
            check("Updates opens its own settings page ('%s')" % head,
                  head == "Updates")
            shot = q.screen_raw()
            check("Updates offers a release check",
                  q.find_ui(scx + GSET_PANE_X,
                            scx + GSET_PANE_X + 152,
                            scy + ui_font_baseline_in(
                                UD_PX, GSET_PANE_Y0 + GSET_INFO_H + 6,
                                GSET_CHIP_H),
                            px=UD_PX,
                            fg=theme("dark")["accent_text"],
                            screen=shot) == "Check for updates")
            check("updates cannot be installed without an installed-system "
                  "boot marker",
                  q.read_ui(scx + GSET_PANE_X,
                            scy + ui_font_baseline_in(
                                11, GSET_PANE_Y0 + GSET_INFO_H + 70, 16),
                            GSET_W - GSET_PANE_X - 8, px=11,
                            fg=theme("dark")["app_dim"],
                            screen=shot) ==
                  "Install only after booting the installed system.")
        finally:
            q.close()
        # --- A second boot: the monitor and the terminal ---
        # The compositor holds at most six windows, and two are taken by
        # the kernel - five applications at once will not fit, so these
        # two get a machine of their own
        print("--- the resource monitor and the terminal in a window ---")
        slog2 = tempfile.mktemp(suffix=".log")
        q2 = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog2, disk=disk,
                  sata=True, qemu="qemu-system-x86_64")
        try:
            q2.wait_serial("thread64>", timeout=60)
            q2.type_line("desktop")
            q2.wait_serial("The desktop is starting", timeout=30)
            check("the desktop comes up a second time", q2.wait_desktop(30))

            q2.mouse_home()
            q2.launch_dock(3)                # the "Monitor" tile
            q2.mouse_goto(400, 300)

            mx, my = body_text(2)
            # The memory capsule (user/gmon.c: 8 tall at content y30,
            # from x12): its track, and as far along as is used, the
            # accent. Both at once mean the numbers arrived and that
            # they are neither zero nor everything.
            dark = theme("dark")
            used = q2.wait_pixel(mx + 14, my + 34, dark["accent"], tol=8,
                                 timeout=20)
            free = q2.wait_pixel(mx + 220 - 16, my + 34, dark["app_row"],
                                 tol=8, timeout=5)
            check("the 'Monitor' tile starts the resource gauge "
                  "(the memory capsule)", used and free)

            # The uptime grows: the window is alive rather than showing one
            # photograph
            def uptime():
                base = my + ui_font_baseline_in(UD_PX, 80, 18)
                got = q2.find_ui(mx + 100, mx + 220, base, px=UD_PX,
                                 fg=dark["app_text"])
                digits = got.replace(" s", "").replace(" ", "")
                return int(digits) if digits.isdigit() else None
            u1 = uptime()
            time.sleep(2.5)
            u2 = uptime()
            check("the monitor is alive (uptime in the window grows: %s -> %s)"
                  % (u1, u2), u1 is not None and u2 is not None and u2 > u1)

            # --- The terminal: keyboard, fork, exec, dup2, a file ---
            q2.mouse_home()
            q2.launch_dock(4)                # the "Term" tile
            q2.mouse_goto(400, 300)

            tx, ty = body_text(3)
            # The prompt on the last line of the window, read in the
            # terminal's own lettering
            rows = []
            end = time.time() + 20
            while time.time() < end:
                rows = q2.term_rows(tx, ty)
                if rows and rows[-1].endswith(">"):
                    break
                time.sleep(0.5)
            check("the 'Term' tile starts the terminal in a window "
                  "(its prompt: %s)" % rows[-1:], bool(rows) and
                  rows[-1].endswith(">"))

            # The terminal's window has focus right after it appears - we
            # type a command and watch whether its output appears in the
            # window. It goes through a file: fork, dup2, exec, wait.
            for ch in "uls64":
                q2.sendkey(ch)
                time.sleep(0.15)
            q2.sendkey("ret")

            listed = []
            end = time.time() + 25
            while time.time() < end:
                listed = [r for r in q2.term_rows(tx, ty) if r]
                if len(listed) > 4:
                    break
                time.sleep(0.5)
            check("a command typed in the terminal prints its result in the "
                  "window (%d lines)" % len(listed), len(listed) > 4)
        finally:
            q2.close()
            if os.path.exists(slog2):
                os.unlink(slog2)
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, disk):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
