#!/usr/bin/env python3
"""ThreadOS smoke test: Alt+F4 and the menu under the right button.

Two gestures that every system a person has used has, and this one did
not:

  ALT+F4 closes the window that has the focus. The keys were already
  arriving - Alt with a function key switched text consoles - but on
  the desktop there is one screen and nothing to switch to, so those
  presses went nowhere. Now the desktop takes them, and F4 means what
  it has meant for thirty years. It closes the window the same way the
  [x] does: the application is TOLD and ends itself, rather than having
  its window pulled out from under it.

  THE RIGHT BUTTON on empty desktop opens the launch menu where the
  pointer is. It is the same list as the mark on the bar opens, because a
  second list would be a second thing to learn - only it comes to the
  hand instead of the hand going to the corner. On a window the right
  button opens nothing (a window's own menu is the window's business)
  and closes a menu that was open.

The assertions are about what a person would notice: the program
started from the menu really runs (it draws), the menu stands at the
pointer and not under the mark on the bar, it never hangs off the screen or
over the dock, Alt+F4 ends the program with its own exit code, and
the window goes from the screen AND from the dock.

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
from ptest import check, summary
from ptest import near as ptest_near

# The cascade: slot i sits at (70+12i, 30+12i), and applications start
# at slot 2 because the kernel's own two windows hold 0 and 1.
WX, WY = 94, 54

# Inside the settings, past both kernel windows - and right of their
# rows, which stop six pixels short of the edge: their background
P_APP = (WX + 1 + GSET_W - 3, 200)


def near(px, v):
    return all(abs(c - v) <= 6 for c in px)


def is_win(px):
    """A window's background in the theme's colour, as the settings
    draw it"""
    return ptest_near(px, theme("dark")["win_bg"], 3)


def is_accent(px):
    return ptest_near(px, theme("dark")["accent"], 12)


def menu_entry(q, mx, my, i):
    """The name on the i-th entry of a menu whose corner is (mx, my) -
    '' where there is no such menu"""
    top = my + MENU_PAD + i * MENU_ROW
    return q.read_ui(mx + 10 + MENU_ICON + 10,
                     ui_font_baseline_in(MENU_PX, top, MENU_ROW),
                     width=110, px=MENU_PX,
                     fg=theme("dark")["menu_text"])


def menu_place(mx, my):
    """Where the menu opened at the pointer really stands: never past
    the right edge, never over the dock (menu_at())"""
    hgt = 2 * MENU_PAD + len(MENU_ITEMS) * MENU_ROW
    x = min(mx, SCREEN_W - MENU_W)
    y = min(my, work_bottom() - hgt)
    return max(x, 0), max(y, BAR_H)


def sixty_four():
    """The same two gestures on the 64-bit kernel. It is worth a boot of
    its own, because the road the keys travel is NOT shared: long mode
    has a keyboard driver of its own, and Alt+F4 had to be let through
    it separately. The compositor above is the same code, so here we ask
    only the one question that differs - do the keys arrive."""
    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("SKIPPED (64): no 64-bit image (make kernel64)")
        return
    if shutil.which("grub-mkrescue") is None or \
       shutil.which("qemu-system-x86_64") is None:
        print("SKIPPED (64): no grub-mkrescue or qemu-system-x86_64")
        return

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)

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
            return

        q = Qemu(KERNEL, INITRD, disk=disk, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=60)
            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=30)
            time.sleep(4)
            q.mouse_home()

            q.launch_dock(1)                # the "Clock" tile
            time.sleep(4)
            q.mouse_goto(600, 400)
            time.sleep(1.5)

            opened = q.dock_windows()
            check("(64) the clock came up from its tile (%d windows in the dock)"
                  % opened, opened == 3)

            # Closing a window is not instant, and how long it takes
            # is not ours to guess: with the whole suite running the
            # same three seconds are sometimes not enough. So we look
            # until the button is gone, and only then give up.
            q.sendkey("alt-f4")
            left = 3
            for _ in range(10):
                time.sleep(1.5)
                left = q.dock_windows()
                if left == 2:
                    break
            check("(64) Alt+F4 gets through the long-mode driver and "
                  "closes the window (%d windows)" % left, left == 2)

            q.mouse_goto(500, 300)
            time.sleep(0.5)
            q.mouse_right_click()
            time.sleep(1.5)
            got = menu_entry(q, 500, 300, 1)
            check("(64) the right button opens the menu under the pointer "
                  "('%s')" % got, got == "Clock")
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, disk):
            if os.path.exists(f):
                os.unlink(f)


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")

    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()

            # --- Alt+F4 -------------------------------------------------
            q.launch_dock(6)                # the "Settings" tile
            time.sleep(4)
            q.mouse_goto(600, 400)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            check("the settings stand on the screen", is_win(at(*P_APP)))
            before = q.dock_windows()
            check("and they have their tile in the dock (%d with the kernel's "
                  "windows)" % before, before == 3)

            q.sendkey("alt-f4")
            time.sleep(3)
            w, h, at = q.screen_raw()

            log = q.serial()
            # Not dropped - ASKED. A window pulled out from under a
            # process leaves it running; this one ended itself, which
            # is what its own exit code says.
            check("Alt+F4 asked the program, and it ended by itself",
                  "process \'gsettings\' has ended (code 0)" in log)
            check("the window is gone from the screen", not is_win(at(*P_APP)))
            after = q.dock_windows()
            check("and it went from the dock (%d -> %d windows)"
                  % (before, after), after == before - 1)

            # Alt+F4 with nothing focused closes nothing - the kernel's
            # own windows are still there
            q.sendkey("alt-f4")
            time.sleep(2)
            w, h, at = q.screen_raw()
            check("Alt+F4 with no focus closes nothing - the kernel's windows "
                  "stand where they stood", q.dock_windows() == after)

            # --- two clicks on the title bar ----------------------------
            # Every system does this, and a person who has used one
            # tries it here in the first minute: the window fills the
            # screen, and does it again the other way round.
            q.launch_dock(6, n_w=2)         # the "Settings" tile
            time.sleep(4)
            q.mouse_goto(600, 200)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("the window opened in its place", is_win(at(*P_APP)))

            q.mouse_goto(WX + 100, WY + 4)
            time.sleep(0.4)
            q.mouse_click()
            time.sleep(0.15)
            q.mouse_click()                    # the second one, quickly
            time.sleep(2.5)
            q.mouse_goto(320, 300)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("two clicks on the title bar open the window to the whole "
                  "screen", is_win(at(20, 300)) and is_win(at(w - 4, 300)))
            check("but not over the dock",
                  min(at(*dock_tile(0, n_w=3))) > 200)

            q.mouse_goto(200, BAR_H + 4)
            time.sleep(0.4)
            q.mouse_click()
            time.sleep(0.15)
            q.mouse_click()
            time.sleep(2.5)
            q.mouse_goto(600, 200)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("and another two clicks give it its own size back",
                  is_win(at(*P_APP)) and not is_win(at(20, 300)))

            q.sendkey("alt-f4")                # away with it again
            time.sleep(3)
            w, h, at = q.screen_raw()
            check("after the close the dock is as it was",
                  q.dock_windows() == after)

            # --- the menu under the right button ------------------------
            MX, MY = 300, 150
            q.mouse_goto(MX, MY)
            time.sleep(0.5)
            q.mouse_right_click()
            time.sleep(1)
            # onto the first entry
            q.mouse_goto(MX + 60, MY + MENU_PAD + MENU_ROW // 2)
            time.sleep(1)
            w, h, at = q.screen_raw()

            # (the first entry is lit under the pointer - read the next two)
            got = menu_entry(q, MX, MY, 1), menu_entry(q, MX, MY, 2)
            check("the right button on the desktop opened the menu UNDER THE "
                  "POINTER (%s)" % (got,), got == ("Clock", "Files"))
            # The pointer sits on the first entry, so that entry wears
            # the accent colour - a menu that does not answer the
            # pointer is a list of words that happens to be clickable
            e0 = MY + MENU_PAD + MENU_ROW // 2
            check("the entry under the pointer is highlighted",
                  is_accent(at(MX + MENU_W - 16, e0)))
            check("and a further one is not",
                  not is_accent(at(MX + MENU_W - 16, e0 + 2 * MENU_ROW)))
            # It came to the hand: nothing opened under the mark on the
            # bar, where the menu stands when the mark is what opened it
            check("and nothing hangs from the mark on the bar",
                  menu_entry(q, MENU_X0, MENU_Y, 0) == "")

            # --- a click on an entry starts the program -----------------
            # The second entry is the clock, and what it draws is a
            # TIME - so a window that merely appeared and a program
            # that actually runs are not the same assertion. We read
            # the digits with the font the kernel drew them with;
            # counting dark pixels would count the kernel's own window
            # underneath, whose button carries dark text too.
            q.mouse_goto(MX + 60, MY + MENU_PAD + MENU_ROW + MENU_ROW // 2)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(4)
            q.mouse_goto(600, 400)
            time.sleep(1.5)
            w, h, at = q.screen_raw()

            check("the menu closed after the choice",
                  menu_entry(q, MX, MY, 0) == "")
            shown = q.gclock_time(WX + 1, WY + CONTENT_DY)
            check("the program from the menu really runs - the clock draws "
                  "godzine ('%s')" % shown, shown.count(":") == 2)
            check("and it has its tile in the dock",
                  q.dock_windows() == after + 1)

            # --- the right button over a window ---------------------------
            q.mouse_goto(500, 350)
            time.sleep(0.5)
            q.mouse_right_click()
            time.sleep(1.5)
            q.mouse_goto(WX + 40, WY + 20)     # inside the clock window
            time.sleep(0.5)
            q.mouse_right_click()
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("the right button on a window does not open the menu",
                  menu_entry(q, *menu_place(WX + 40, WY + 20), 0) == "")
            check("and it closes the one that was open",
                  menu_entry(q, *menu_place(500, 350), 0) == "")

            # --- the menu never hangs off the screen ----------------------
            # Clicked in the corner it slides back: to the left of the
            # right-hand edge, and up off the dock
            q.mouse_goto(620, 400)
            time.sleep(0.5)
            q.mouse_right_click()
            time.sleep(1.5)
            mx, my = menu_place(620, 400)
            got = menu_entry(q, mx, my, 0), menu_entry(q, mx, my, 6)
            check("the menu at the edge moved left and up, and is whole there "
                  "(%s at %d,%d)" % (got, mx, my), got == ("App", "Mixer"))
            check("it does not go past the right edge, nor over the dock",
                  mx + MENU_W <= SCREEN_W and
                  my + 2 * MENU_PAD + len(MENU_ITEMS) * MENU_ROW <= work_bottom())
            q.mouse_goto(600, 140)
            q.mouse_click()                    # away with it
            time.sleep(1)
            q.mouse_goto(WX + 72, WY + 4)      # past its lights
            q.mouse_click()                    # the clock has the focus
            time.sleep(1)

            # --- and Alt+F4 on a window that came from the menu ----------
            q.sendkey("alt-f4")
            time.sleep(3)
            w, h, at = q.screen_raw()
            log = q.serial()
            check("Alt+F4 closes a window started from the menu too",
                  "process \'gclock\' has ended (code 0)" in log)
            gone = q.gclock_time(WX + 1, WY + CONTENT_DY)
            check("the clock is gone from the screen ('%s')" % gone,
                  gone.count(":") != 2)
            check("and from the dock", q.dock_windows() == after)
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    print("--- the same on the 64-bit kernel ---")
    sixty_four()
    return summary()


sys.exit(main())
