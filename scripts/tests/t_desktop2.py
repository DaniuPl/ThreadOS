#!/usr/bin/env python3
"""ProtonOS smoke test: the file manager, icons, the monitor, the
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
from ptest import check, summary


def main():
    # --- The GUI file manager: FS -> an application window (a separate
# boot with a disk) ---
    print("--- pulpit: menedzer plikow (gfiles) ---")
    gdisk = tempfile.mktemp(suffix=".img")
    with open(gdisk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    q9 = Qemu(KERNEL, INITRD, gdisk)
    try:
        q9.wait_desktop()
        q9.type_line("write raport abc")   # a file on the disk (before the desktop)
        time.sleep(0.5)
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # Menu -> the 'gfiles' entry
        q9.mouse_goto(10, 472)
        q9.mouse_click()
        q9.mouse_goto(40, menu_y("gfiles"))
        q9.mouse_click()
        # gfiles slot2 (94,54), content (95,65); the file name at
        # content(8,16) -> screen(103,81). The text (white) = files from
        # PFS.
        def files_row_white():
            _, _, a = q9.screen_raw()
            return sum(1 for x in range(103, 160) for y in range(81, 89)
                       if near(a(x, y), (255, 255, 255), 10))
        appeared = False
        for _ in range(15):
            if files_row_white() > 0:
                appeared = True
                break
            time.sleep(0.2)
        check("menedzer plikow GUI listuje plik z dysku (FS -> okno)",
              appeared)

        # A click on the file name -> a preview of the contents. Line 0 at
        # content(.,16) -> screen y81, the next ones every 10 px. We click
        # LINE 1: line 0 is 'swap.sys', which the kernel creates at
        # startup (inode 0), and what we want is 'raport'. After the click
        # the panel 'tresc:' (content y102 -> screen y167) shows the
        # contents. We move the cursor away before the screenshot.
        q9.mouse_goto(115, 93)
        q9.mouse_click()
        q9.mouse_goto(60, 300)

        def preview_white():
            _, _, a = q9.screen_raw()
            return sum(1 for x in range(99, 200) for y in range(167, 175)
                       if near(a(x, y), (255, 255, 255), 10))
        prev = False
        for _ in range(15):
            if preview_white() > 0:
                prev = True
                break
            time.sleep(0.2)
        check("klik pliku w gfiles pokazuje jego tresc (open+read z ring3)",
              prev)
    finally:
        q9.close()
        if os.path.exists(gdisk):
            os.unlink(gdisk)

    # --- Icons on the desktop (a separate boot) ---
    print("--- pulpit: ikony ---")
    q10 = Qemu(KERNEL, INITRD)
    try:
        q10.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # the 'App' icon (the tile 8,20,32x24) in orange
        check("ikona pulpitu narysowana (pomaranczowy kafelek 20,30)",
              q10.wait_pixel(20, 30, (255, 162, 0)))
        # a click on the icon starts the application (guiapp -> a red square)
        q10.mouse_goto(24, 32)
        q10.mouse_click()
        q10.mouse_goto(300, 300)         # move the cursor away
        check("klik ikony uruchamia aplikacje (czerwony kwadrat 101,71)",
              q10.wait_pixel(101, 71, (255, 0, 0)))
    finally:
        q10.close()

    # --- The resource monitor (gmon) live ---
    print("--- pulpit: monitor zasobow (gmon) ---")
    q11 = Qemu(KERNEL, INITRD)
    try:
        q11.wait_desktop()
        time.sleep(0.8)
        q11.type_line("run gmon")

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # gmon slot2 (94,54), content (95,65). The RAM bar (content y48)
        # contains a blue usage fragment -> screen ~ (101,116).
        check("monitor zasobow: pasek RAM (niebieski 101,116)",
              q11.wait_pixel(101, 116, (0, 0, 255)))

        # the uptime (the number at content(96,28) -> screen (191,93)) grows:
        # the text cell changes over time -> a live monitor
        def up_cell():
            _, _, a = q11.screen_raw()
            return [a(191 + c, 93 + r) for r in range(8) for c in range(24)]
        c1 = up_cell()
        time.sleep(1.6)
        c2 = up_cell()
        check("monitor zasobow zywy: uptime aktualizuje sie w oknie",
              c1 != c2)
    finally:
        q11.close()

    # --- A terminal in a GUI window (a separate boot with a disk) ---
    print("--- pulpit: terminal GUI (gterm) ---")
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
            return sum(1 for x in range(99, 130) for y in range(69, 99)
                       if near(a(x, y), (255, 255, 255), 10))
        shown = False
        for _ in range(20):
            if out_white() > 0:
                shown = True
                break
            time.sleep(0.2)
        check("terminal GUI uruchamia komende i pokazuje wyjscie w oknie",
              shown)
    finally:
        q12.close()
        if os.path.exists(tdisk):
            os.unlink(tdisk)

    # --- Collapsing and resizing windows (a separate boot) ---
    print("--- pulpit: minimalizacja + zmiana rozmiaru ---")
    q13 = Qemu(KERNEL, INITRD)
    try:
        q13.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # win0 "Terminal" (40,60,120x70). A title bar is the accent
        # blue for the window with the focus and plain grey for the
        # rest; at boot nothing has the focus. Read past the name.
        check("okno 0: pasek tytulu narysowany przed zwinieciem (115,64)",
              q13.wait_pixel(115, 64, (178, 178, 178), 25))

        # win0's collapse button [_]: min_x = x+w-2*8-2 = 142, y=61 (8x8).
        # A click on the centre (146,65) collapses the window -> the bar
        # disappears, the background comes back.
        q13.mouse_goto(146, 65)
        q13.mouse_click()
        q13.mouse_goto(400, 300)              # move the cursor away
        check("zwiniete okno znika z pulpitu (pasek 80,64 -> teal)",
              q13.wait_pixel(80, 64, (0, 128, 160)))

        # The window is still on the taskbar (slot0 'Terminal', x46..96,
        # y468). A click restores it: the blue title bar is visible again.
        q13.mouse_goto(60, 473)
        q13.mouse_click()
        q13.mouse_goto(400, 300)
        # Restoring it from the bar also gives it the focus, so the bar
        # comes back BLUE
        check("klik na pasku zadan przywraca zwiniete okno (115,68)",
              q13.wait_pixel(115, 68, (0, 0, 255), 30))

        # Resizing: win0's grip in the bottom right corner
        # grip_x = x+w-8 = 152, grip_y = y+h-8 = 122. Before the change
        # the point (170,140) is outside the window (teal). We drag the
        # grip (155,125) by (+60,+40) -> the window grows and covers
        # (170,140).
        _, _, at = q13.screen_raw()
        check("przed zmiana rozmiaru punkt poza oknem (170,140 teal)",
              near(at(170, 140), (0, 128, 160)))
        q13.mouse_goto(155, 125)
        q13.mouse_button(True)
        q13.mouse_move(60, 40)
        q13.mouse_button(False)
        q13.mouse_goto(400, 300)
        check("po zmianie rozmiaru okno powiekszone (170,140 szare tlo okna)",
              q13.wait_pixel(170, 140, (224, 224, 224)))
    finally:
        q13.close()

    # --- Saving the icon positions on the disk (two boots on the same
# disk) ---
    print("--- pulpit: zapis pozycji ikon na dysku ---")
    idisk = tempfile.mktemp(suffix=".img")
    with open(idisk, "wb") as f:
        f.write(b"\0" * DISK_SIZE)
    q14 = Qemu(KERNEL, INITRD, idisk)
    try:
        q14.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # the 'App' icon by default (8,20,32x24) - an orange tile ~ (20,30)
        check("ikona App w domyslnej pozycji (20,30 pomaranczowy)",
              q14.wait_pixel(20, 30, (255, 162, 0)))

        # Drag the icon: grab the centre (24,32), move it to (416,312) with
        # the left button held -> the icon lands at (400,300). Release.
        q14.mouse_goto(24, 32)
        q14.mouse_button(True)
        q14.mouse_goto(416, 312)
        q14.mouse_button(False)
        q14.mouse_goto(100, 250)          # move the cursor away from the icon
        # The tile, off its picture: the icon now has one drawn in the
        # middle of it, and that is not orange
        check("ikona przeniesiona na nowa pozycje (404,304 pomaranczowy)",
              q14.wait_pixel(404, 304, (255, 162, 0)))
        check("stara pozycja ikony wyczyszczona (20,30 teal)",
              q14.wait_pixel(20, 30, (0, 128, 160)))
    finally:
        q14.close()

    # A reboot on THE SAME disk: the icon's position survived (icons.pos)
    q15 = Qemu(KERNEL, INITRD, idisk)
    try:
        q15.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        check("pozycja ikony przetrwala restart (404,304 pomaranczowy)",
              q15.wait_pixel(404, 304, (255, 162, 0)))
        check("domyslna pozycja ikony pusta po restarcie (20,30 teal)",
              q15.wait_pixel(20, 30, (0, 128, 160)))
    finally:
        q15.close()
        if os.path.exists(idisk):
            os.unlink(idisk)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
