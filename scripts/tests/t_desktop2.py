#!/usr/bin/env python3
"""ThreadOS smoke test: the file manager, the dock, the monitor, the
terminal, window sizes.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403

# These positions were measured when the title bar was ten pixels tall;
# what lies under a window's bar has moved down by the difference
DOWN = TITLE_H - 10
from ptest import check, summary


def main():
    # --- The GUI file manager: FS -> an application window (a separate
# boot with a disk) ---
    print("--- the desktop: the file manager (gfiles) ---")
    gdisk = tempfile.mktemp(suffix=".img")
    with open(gdisk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    q9 = Qemu(KERNEL, INITRD, gdisk)
    try:
        q9.wait_desktop()
        q9.type_line("write report abc")   # a file on the disk (before the desktop)
        time.sleep(0.5)
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # Menu -> the 'gfiles' entry
        q9.launch_menu("gfiles")
        # gfiles slot2 (94,54), content (95,65); the file name at
        # content(8,16) -> screen(103,81). The text is BLACK - the
        # window is light grey, and white on it was unreadable.
        def files_row_white():
            _, _, a = q9.screen_raw()
            return sum(1 for x in range(103, 160) for y in range(81 + DOWN, 89 + DOWN)
                       if near(a(x, y), (0, 0, 0), 10))
        appeared = False
        for _ in range(15):
            if files_row_white() > 0:
                appeared = True
                break
            time.sleep(0.2)
        check("the GUI file manager lists a file from the disk (FS -> a window)",
              appeared)

        # A click on a file name chooses it: the row takes the accent
        # colour, which is how everything on this desktop says "this
        # one". The contents are no longer shown here - a file manager
        # hands a file to the program that opens it, and that program
        # is the editor.
        #
        # Row 1 (content y16 + 10 -> screen y91): row 0 is 'swap.sys',
        # which the kernel makes at startup, and what we want is
        # 'raport'.
        def row_blue():
            _, _, a = q9.screen_raw()
            return sum(1 for x in range(100, 300) for y in range(91 + DOWN, 99 + DOWN)
                       if near(a(x, y), (0, 0, 255), 60))

        before_blue = row_blue()
        q9.mouse_goto(115, 93 + DOWN)
        q9.mouse_click()
        q9.mouse_goto(60, 300)
        picked = False
        for _ in range(15):
            if row_blue() > before_blue + 100:
                picked = True
                break
            time.sleep(0.2)
        check("a click on a file in gfiles picks it (a mouse event -> ring 3)",
              picked)
    finally:
        q9.close()
        if os.path.exists(gdisk):
            os.unlink(gdisk)

    # --- The dock (a separate boot) ---
    print("--- the desktop: the dock ---")
    q10 = Qemu(KERNEL, INITRD)
    try:
        q10.wait_desktop()
        time.sleep(1.0)
        # The first program's tile: a picture on a tile of its own - a
        # blue slope with a white window on it. Its top edge is the
        # slope's light end, its middle the white of the window.
        tx, ty = dock_geom()["tiles"][0]
        t = dock_geom()["tile"]
        _, _, at = q10.screen_raw()
        top, mid = at(tx + 3, ty + 3), at(tx + t // 2, ty + t // 2 + 3)
        check("the App tile is drawn in the dock (%s at its top, %s in its "
              "middle)" % (top, mid),
              top[2] > 200 and top[0] < 160 and min(mid) > 200)
        # ... and its corners are round: the corner pixel is the glass
        check("the tile's corners are round (%s at the very corner)"
              % (at(tx, ty),), not near(at(tx, ty), top, 40))
        # a click on the tile starts the application (guiapp -> a red square)
        q10.launch_dock(0)
        q10.mouse_goto(300, 300)         # move the cursor away
        check("a click on a tile starts the application (a red square 101,71)",
              q10.wait_pixel(101, 71 + DOWN, (255, 0, 0)))
    finally:
        q10.close()

    # --- The resource monitor (gmon) live ---
    print("--- the desktop: the resource monitor (gmon) ---")
    q11 = Qemu(KERNEL, INITRD)
    try:
        q11.wait_desktop()
        time.sleep(0.8)
        q11.type_line("run gmon")

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # gmon slot2 (94,54), content (95,65). The RAM bar (content y48)
        # contains a blue usage fragment -> screen ~ (101,116).
        check("the resource monitor: the RAM bar (blue 101,116)",
              q11.wait_pixel(101, 116 + DOWN, (0, 0, 255)))

        # the uptime (the number at content(96,28) -> screen (191,93)) grows:
        # the text cell changes over time -> a live monitor
        def up_cell():
            _, _, a = q11.screen_raw()
            return [a(191 + c, 93 + DOWN + r) for r in range(8) for c in range(24)]
        c1 = up_cell()
        time.sleep(1.6)
        c2 = up_cell()
        check("the resource monitor is alive: uptime updates in its window",
              c1 != c2)
    finally:
        q11.close()

    # --- A terminal in a GUI window (a separate boot with a disk) ---
    print("--- the desktop: a terminal in a window (gterm) ---")
    tdisk = tempfile.mktemp(suffix=".img")
    with open(tdisk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    q12 = Qemu(KERNEL, INITRD, tdisk)
    try:
        q12.wait_desktop()
        time.sleep(0.8)
        q12.type_line("run gterm")
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # a click in the terminal's window (slot2, content 95,65) -> keyboard
# focus
        q12.mouse_goto(150, 120)
        q12.mouse_click()
        q12.mouse_goto(400, 300)
        # type 'seq 3' and Enter -> the output (1,2,3) appears in the window
        for ch in "seq 3":
            q12.sendkey({" ": "spc"}.get(ch, ch))
        q12.sendkey("ret")

        # the output is drawn at the top of the window (content y4 -> screen
# y69)
        def out_white():
            _, _, a = q12.screen_raw()
            return sum(1 for x in range(99, 130) for y in range(69 + DOWN, 99 + DOWN)
                       if near(a(x, y), (255, 255, 255), 10))
        shown = False
        for _ in range(20):
            if out_white() > 0:
                shown = True
                break
            time.sleep(0.2)
        check("the GUI terminal runs a command and shows the output in its window",
              shown)
    finally:
        q12.close()
        if os.path.exists(tdisk):
            os.unlink(tdisk)

    # --- Collapsing and resizing windows (a separate boot) ---
    print("--- the desktop: minimising + resizing ---")
    q13 = Qemu(KERNEL, INITRD)
    try:
        q13.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        dark = theme("dark")
        # win0 "Terminal" (40,60,120x70)
        _, _, at = q13.screen_raw()
        check("window 0: the title bar is drawn before the minimise (150,63)",
              near(at(150, 63), dark["title_off"], 3))

        # win0's yellow light minimises it: the window goes, the aurora
        # comes back
        q13.click(*light_xy(40, 60, 1))
        q13.mouse_goto(400, 300)              # move the cursor away
        check("a minimised window goes from the desktop (the aurora at 80,64)",
              q13.wait_background(80, 64, tol=0))

        # The window is still in the dock (the first window's tile). A
        # click brings it back, with the focus.
        q13.click(*dock_window(0, 2))
        q13.mouse_goto(400, 300)
        check("a click on its tile in the dock brings a minimised window back",
              q13.wait_focus(40, 60))

        # Resizing: win0's grip in the bottom right corner (the last ten
        # pixels each way). Before, (170,140) is outside the window - its
        # shadow at most. We drag the grip (155,125) by (+60,+40) -> the
        # window grows and covers (170,140).
        _, _, at = q13.screen_raw()
        check("before the resize the point is outside the window (170,140)",
              not near(at(170, 140), dark["win_bg"], 4))
        q13.mouse_goto(155, 125)
        q13.mouse_button(True)
        q13.mouse_move(60, 40)
        q13.mouse_button(False)
        q13.mouse_goto(400, 300)
        check("after the resize the window is bigger (170,140 the window's body)",
              q13.wait_pixel(170, 140, dark["win_bg"], 3))
    finally:
        q13.close()

    return summary()


if __name__ == "__main__":
    sys.exit(main())
