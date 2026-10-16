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
BIALY = (255, 255, 255)
CZARNY = (0, 0, 0)


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
            q.mouse_goto(24, 32)          # the 'App' icon (the tile 8,20,32x24)
            q.mouse_click()
            q.mouse_goto(400, 300)        # move the cursor away from the window

            # The application draws a palette of three squares at the top
            # of the window. Each from a different system call, so each
            # separately.
            cx, cy = scr(10, 10)
            check("a click on an icon starts a program from the initrd (a red square)",
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
            _, _, at = q.screen_raw()
            tx, ty = scr(6, 24)
            whites = sum(1 for dy in range(8) for dx in range(72)
                          if at(tx + dx, ty + dy) == CZARNY)
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
            # The 'Zegar' icon stands at (8,60), a 32x24 tile
            q.mouse_home()
            q.mouse_goto(24, 72)
            q.mouse_click()
            q.mouse_goto(400, 300)

            zx, zy = body_text(3)
            def clock_window(fun):
                return [fun(zx + 8 + dx, zy + 4 + dy)
                        for dy in range(8) for dx in range(64)]

            clock_present = False
            end = time.time() + 20
            while time.time() < end:
                _, _, az = q.screen_raw()
                if any(p != BIALY and p != (227, 227, 227)
                       for p in clock_window(az)):
                    clock_present = True
                    break
                time.sleep(0.5)
            check("the 'Clock' icon starts a second application in a window of its own",
                  clock_present)

            # The clock is to run: the time changes in the window of its own
# accord
            _, _, az1 = q.screen_raw()
            first = clock_window(az1)
            time.sleep(2.5)
            _, _, az2 = q.screen_raw()
            check("the clock in the window runs (its content changes by itself)",
                  clock_window(az2) != first)

            # --- Files: a third application, it reads the disk ---
            q.mouse_home()
            q.mouse_goto(24, 112)
            q.mouse_click()
            q.mouse_goto(400, 300)

            px2, py2 = body_text(4)
            # The heading - the directory being shown and how many
            # things are in it - is drawn in the accent colour at (4,2)
            greens = 0
            end = time.time() + 20
            while time.time() < end:
                _, _, ap = q.screen_raw()
                greens = sum(1 for dy in range(8) for dx in range(120)
                                if ap(px2 + 4 + dx, py2 + 2 + dy) == BLUE)
                if greens > 10:
                    break
                time.sleep(0.5)
            check("the 'Files' icon starts the file manager "
                  "(%d title-bar pixels in the accent colour)" % greens,
                  greens > 10)

            # A click chooses a file: the row takes the accent colour,
            # which is this desktop's way of saying "this one". That is
            # the whole path being checked - a mouse event reaching a
            # ring 3 program and coming back as a drawing.
            def row_blue(row):
                _, _, a = q.screen_raw()
                return sum(1 for dx in range(200) for dy in range(8)
                           if a(px2 + 4 + dx, py2 + 18 + row * 10 + dy)
                           == BLUE)

            picked = False
            for row in range(4):
                before = row_blue(row)
                q.mouse_home()
                q.mouse_goto(px2 + 20, py2 + 18 + row * 10 + 4)
                q.mouse_click()
                q.mouse_goto(400, 300)
                time.sleep(1.5)
                if row_blue(row) > before + 100:
                    picked = True
                    break
            check("a click on a file name picks it in the manager", picked)

            # The compositor survived all this: the desktop's background is still
# there.
            w, h, at4 = q.screen_raw()
            tlo = at4(w - 20, h // 2)
            check("the compositor still draws the desktop",
                  all(abs(tlo[i] - (0, 130, 162)[i]) <= 40 for i in range(3)))
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

            # The 'Monitor' icon at (8,140)
            q2.mouse_home()
            q2.mouse_goto(24, 152)
            q2.mouse_click()
            q2.mouse_goto(400, 300)

            mx, my = body_text(2)
            # The memory usage bar: a red background, the used part in
            # blue. Both colours at once mean the numbers arrived and
            # that they are neither zero nor everything.
            red = q2.wait_pixel(mx + 150, my + 52, RED, timeout=20)
            blue = q2.wait_pixel(mx + 6, my + 52, BLUE, timeout=5)
            check("the 'Monitor' icon starts the resource gauge "
                  "(the memory bar)", red and blue)

            # The uptime grows: the window is alive rather than showing one
# photograph
            def pole_uptime(fun):
                return [fun(mx + 96 + dx, my + 28 + dy)
                        for dy in range(8) for dx in range(40)]

            _, _, am1 = q2.screen_raw()
            u1 = pole_uptime(am1)
            time.sleep(2.5)
            _, _, am2 = q2.screen_raw()
            check("the monitor is alive (uptime in the window grows)",
                  pole_uptime(am2) != u1)

            # --- The terminal: keyboard, fork, exec, dup2, a file ---
            q2.mouse_home()
            q2.mouse_goto(24, 192)          # the 'Term' icon at (8,180)
            q2.mouse_click()
            q2.mouse_goto(400, 300)

            tx, ty = body_text(3)
            # The prompt in green on the last line of the window. The
            # terminal's content is 400x240, its rows are 10 apart and
            # the line being edited is the last of them: 4 + 22*10.
            # We count the pixels of the whole character cell instead of
            # aiming at one - the shape of a glyph is not a thing to
            # guess at.
            prompts = 0
            end = time.time() + 20
            while time.time() < end:
                _, _, ah = q2.screen_raw()
                prompts = sum(1 for dy in range(8) for dx in range(8)
                              if ah(tx + 4 + dx, ty + 224 + dy) == GREEN)
                if prompts > 3:
                    break
                time.sleep(0.5)
            check("the 'Term' icon starts the terminal in a window "
                  "(the prompt character: %d pixels)" % prompts, prompts > 3)

            # The terminal's window has focus right after it appears - we
            # type a command and watch whether its output appears in the
            # window. It goes through a file: fork, dup2, exec, wait.
            def result_field(fun):
                return sum(1 for dy in range(40) for dx in range(200)
                           if fun(tx + 4 + dx, ty + 4 + dy) == BIALY)

            _, _, at1 = q2.screen_raw()
            empty = result_field(at1)
            for ch in "uls64":
                q2.sendkey(ch)
                time.sleep(0.15)
            q2.sendkey("ret")

            printed = 0
            end = time.time() + 25
            while time.time() < end:
                _, _, at2 = q2.screen_raw()
                printed = result_field(at2)
                if printed > empty + 100:
                    break
                time.sleep(0.5)
            check("a command typed in the terminal prints its result in the window "
                  "(%d white pixels)" % printed, printed > empty + 100)
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
