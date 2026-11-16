#!/usr/bin/env python3
"""ThreadOS smoke test: the graphical desktop, started from the shell.

The machine boots to the text shell on a framebuffer GRUB set up, and
the shell's 'desktop' command hands the screen and the keyboard over to
the compositor (kernel/desktop.c). The desktop starts applications by
name through the kernel's compatibility dictionary and takes the
keyboard over from the shell - and gives back the keys no window wants.

We check what can be seen: the background, the bar, the dock, the
windows, the focus after a click and that the cursor moves with the
mouse - and then the files on the Desktop, the desktop's own menu, and
the account manager reaching the kernel through its system call.
"""

import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64


def main():
    print("--- the graphical desktop, started from the shell ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return 0

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    try:
        # A framebuffer from GRUB, and the shell on it: the desktop is
        # started by a command, not by the kernel's command line
        made = make_iso64(d, iso, graphics=True)
        check("the disc with the kernel builds", made)
        if not made:
            return summary()

        q = Qemu(KERNEL, INITRD, disk=disk, cdrom=iso, serial=slog)
        try:
            q.wait_serial("thread64>", timeout=60)
            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=30)
            check("the account sign-in completes", q.wait_desktop())
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
            check("the moved Home shortcut opens Files "
                  "('%s')" % title, "Files: /home/" in title)
            q.click(*light_xy(94, 54, 0))
            deadline = time.time() + 5
            while time.time() < deadline and q.dock_windows():
                time.sleep(0.25)
            check("closing Files leaves the desktop clear",
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
            check("the desktop menu creates a folder (%s)" %
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
            check("the desktop menu creates a text file (%s)" %
                  desktop_rows, "New Text File.txt" in desktop_rows and
                  "New Folder" in desktop_rows)
            q.sendkey("alt-f4")
            deadline = time.time() + 8
            while time.time() < deadline and q.dock_windows() != baseline:
                time.sleep(0.3)

            # The mouse: the cursor is to go where we move it. We check
            # that through a change of pixels at the destination.
            q.mouse_goto(400, 300)
            differ = 0
            deadline = time.time() + 8
            while time.time() < deadline:
                _, _, at2 = q.screen_raw()
                differ = sum(1 for dy in range(12) for dx in range(12)
                             if at2(400 + dx, 300 + dy) !=
                             at(400 + dx, 300 + dy))
                if differ > 8:
                    break
                time.sleep(0.25)
            check(f"the pointer follows the mouse ({differ} pixels changed)",
                  differ > 8)

            # A key that no window wants goes to the kernel's shell.
            # Without somewhere to hand those keys back to, a desktop
            # full of windows would be a screen nobody could type at.
            q.mouse_goto(560, 300)
            q.mouse_button(True)          # the desktop takes the focus
            time.sleep(0.3)
            q.mouse_button(False)
            time.sleep(1)
            out = q.command("uptime", "Uptime:", timeout=20)
            check("a key no window wants reaches the "
                  "kernel's shell", "Uptime:" in out)

            # The account manager crosses the user ABI to enumerate the
            # persistent accounts.
            users_w, users_h = 520, 340
            q.launch_menu("gusers")
            ux, uy = cascade(2, users_w + 2,
                             users_h + TITLE_H + 2)

            def read_until(read, want, timeout=20):
                # A loaded machine can take seconds to start a program
                # and draw its first frame
                got = None
                end = time.time() + timeout
                while time.time() < end:
                    got = read()
                    if got == want:
                        break
                    time.sleep(0.25)
                return got

            users_title = read_until(
                lambda: q.window_title(ux, uy, users_w + 2), "User Accounts")
            check("the account manager opens ('%s')" % users_title,
                  users_title == "User Accounts")
            cx, cy = ux + 1, uy + CONTENT_DY
            row_base = cy + ui_font_baseline_in(UD_PX, 96, 18)
            first_account = read_until(
                lambda: q.read_ui(cx + 24, row_base, 100,
                                  charset="admin", px=UD_PX), "admin")
            check("the account syscall returns the admin account "
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
            second_base = cy + ui_font_baseline_in(UD_PX, 118, 18)
            second_account = read_until(
                lambda: q.read_ui(cx + 24, second_base, 100, px=UD_PX),
                "gui64", timeout=15)
            check("the account manager creates an account through the syscall "
                  "('%s')" % second_account,
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
