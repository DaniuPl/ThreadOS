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
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
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
            check("the account sign-in completes", q.wait_desktop(40))
            time.sleep(4)

            q.click_desktop()
            q.type_line("write /home/admin/Desktop/desk.txt visible")
            deadline = time.time() + 10
            desk_label = ""
            while time.time() < deadline:
                w, h, at = q.screen_raw()
                desk_label = q.find_ui(
                    270, 352,
                    ui_font_baseline_in(10, BAR_H + 8 + 43, 20),
                    charset="desk.txt", px=10,
                    under=lambda x, y: aurora_px(x, y, w, h),
                    screen=(w, h, at))
                if "desk.txt" in desk_label:
                    break
                time.sleep(0.25)
            check("the signed-in user's Desktop is enumerated after setup "
                  "('%s')" % desk_label, "desk.txt" in desk_label)

            q.mouse_goto(0, 0)
            time.sleep(0.3)
            w, h, at = q.screen_raw()
            check(f"the desktop comes up in graphics mode ({w}x{h})",
                  w >= 640 and h >= 400)
            mid = (w // 2, h // 2)
            check("the aurora fills the screen (%s at the middle, %s expected)"
                  % (at(*mid), aurora_px(*mid, w=w, h=h)),
                  at(*mid) == aurora_px(*mid, w=w, h=h))
            bar_pixel = at(w // 2, 4)
            bar_expected = bar_glass(True, w, h)[4][w // 2]
            check("the menu bar is drawn - the aurora through glass "
                  "(%s vs %s)" % (bar_pixel, bar_expected),
                  near(bar_pixel, bar_expected, 1))
            tile = dock_tile(0, n_w=0, w=w, h=h)
            tile_pixel = at(*tile)
            tile_background = aurora_px(*tile, w=w, h=h)
            tile_contrast = max(abs(tile_pixel[i] - tile_background[i])
                                for i in range(3))
            check("the first dock tile is visible (%s vs background %s)"
                  % (tile_pixel, tile_background), tile_contrast > 32)

            # The desktop starts with no placeholder windows.
            check("no demo windows cover the desktop",
                  at(130, 120) == aurora_px(130, 120, w=w, h=h) and
                  at(240, 140) == aurora_px(240, 140, w=w, h=h))

            # Moving a shortcut must not turn its release into a click;
            # after the move, a double-click at its new position opens it.
            icon_y = BAR_H + 42
            q.mouse_goto(48, icon_y)
            q.mouse_button(True)
            q.mouse_goto(338, 190)
            q.mouse_button(False)
            q.mouse_goto(338, 190)
            time.sleep(0.4)
            q.mouse_click()
            q.mouse_click()
            title = ""
            deadline = time.time() + 10
            while time.time() < deadline:
                title = q.window_title(94, 54)
                if "Files: /home/" in title:
                    break
                time.sleep(0.25)
            check("the moved Home shortcut opens Files in the 64-bit "
                  "desktop ('%s')" % title, "Files: /home/" in title)
            q.click(*light_xy(94, 54, 0))
            deadline = time.time() + 5
            while time.time() < deadline and q.dock_windows():
                time.sleep(0.25)
            check("closing Files leaves the 64-bit desktop clear",
                  q.dock_windows() == 0)

            # A Desktop file is opened through gfiles with both its raw
            # directory index and the Desktop path as arguments.
            file_x = 10 + 3 * 88 + 38
            file_y = BAR_H + 42
            q.mouse_goto(file_x, file_y)
            time.sleep(0.4)
            q.mouse_click()
            q.mouse_click()
            q.mouse_goto(w - 8, h - 8)
            title = ""
            deadline = time.time() + 10
            while time.time() < deadline:
                for wx, wy in ((94, 54), (106, 66), (118, 78)):
                    title = q.window_title(wx, wy)
                    if "desk.txt" in title:
                        break
                if "desk.txt" in title:
                    break
                time.sleep(0.25)
            check("double-clicking a Desktop file opens it in the editor "
                  "('%s')" % title, "desk.txt" in title)
            editor_xy = next(
                ((wx, wy) for wx, wy in ((94, 54), (106, 66), (118, 78))
                 if "desk.txt" in q.window_title(wx, wy)),
                None)
            if editor_xy:
                q.click(*light_xy(*editor_xy, 0))
            deadline = time.time() + 8
            while time.time() < deadline and q.dock_windows() > 1:
                time.sleep(0.25)
            if q.window_title(94, 54).startswith("Files:"):
                q.click(*light_xy(94, 54, 0))
            deadline = time.time() + 8
            while time.time() < deadline and q.dock_windows():
                time.sleep(0.25)
            check("closing the opened Desktop file returns to the desktop",
                  q.dock_windows() == 0)

            def create_from_desktop_menu(row):
                mx, my = 300, 120
                q.mouse_goto(mx, my)
                time.sleep(0.3)
                q.mouse_right_click()
                time.sleep(0.5)
                q.click(mx + 60,
                        my + MENU_PAD + row * MENU_ROW + MENU_ROW // 2)

            baseline = q.dock_windows()
            create_from_desktop_menu(0)
            desktop_rows = []
            deadline = time.time() + 8
            while time.time() < deadline:
                if q.dock_windows() > baseline:
                    desktop_rows = [
                        name for name, _, _ in q.gfiles_rows(
                            95, 54 + CONTENT_DY)]
                    if "New Folder" in desktop_rows:
                        break
                time.sleep(0.3)
            check("the 64-bit desktop menu creates a folder (%s)" %
                  desktop_rows, "New Folder" in desktop_rows)
            q.sendkey("alt-f4")
            deadline = time.time() + 8
            while time.time() < deadline and q.dock_windows() != baseline:
                time.sleep(0.3)

            create_from_desktop_menu(1)
            desktop_rows = []
            deadline = time.time() + 8
            while time.time() < deadline:
                if q.dock_windows() > baseline:
                    desktop_rows = [
                        name for name, _, _ in q.gfiles_rows(
                            95, 54 + CONTENT_DY)]
                    if "New Text File.txt" in desktop_rows:
                        break
                time.sleep(0.3)
            check("the 64-bit desktop menu creates a text file (%s)" %
                  desktop_rows, "New Text File.txt" in desktop_rows and
                  "New Folder" in desktop_rows)
            q.sendkey("alt-f4")
            deadline = time.time() + 8
            while time.time() < deadline and q.dock_windows() != baseline:
                time.sleep(0.3)

            # The mouse: the cursor is to go where we move it. We check
            # that through a change of pixels at the destination.
            q.mouse_goto(400, 300)
            time.sleep(1.5)
            _, _, at2 = q.screen_raw()
            differ = sum(1 for dy in range(12) for dx in range(12)
                        if at2(400 + dx, 300 + dy) != at(400 + dx, 300 + dy))
            check(f"the pointer follows the mouse ({differ} pixels changed)",
                  differ > 8)

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

            # The shared account manager also crosses the 64-bit user
            # ABI to enumerate the persistent accounts.
            users_w, users_h = 520, 340
            q.launch_menu("gusers")
            time.sleep(2)
            ux, uy = cascade(2, users_w + 2,
                             users_h + TITLE_H + 2)
            check("the account manager opens in long mode",
                  q.window_title(ux, uy, users_w + 2) == "User Accounts")
            cx, cy = ux + 1, uy + CONTENT_DY
            row_base = cy + ui_font_baseline_in(UD_PX, 96, 18)
            first_account = q.read_ui(cx + 24, row_base, 100,
                                      charset="admin", px=UD_PX)
            check("the 64-bit account syscall returns the admin account "
                  "('%s')" % first_account,
                  first_account == "admin")

            def type_text(text):
                for ch in text:
                    q.sendkey(q.KEYNAMES.get(ch, ch))
                    time.sleep(0.12)

            q.click(cx + 300, cy + 105)
            type_text("gui64")
            q.click(cx + 300, cy + 157)
            type_text("gui64pass")
            q.click(cx + 300, cy + 209)
            type_text("gui64pass")
            q.click(cx + 300, cy + 261)
            type_text("thread")
            q.click(cx + 72, cy + users_h - 28)
            time.sleep(2)
            second_base = cy + ui_font_baseline_in(UD_PX, 118, 18)
            second_account = q.read_ui(cx + 24, second_base, 100, px=UD_PX)
            check("the 64-bit account manager creates an account through the syscall",
                  second_account == "gui64")
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, disk):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
