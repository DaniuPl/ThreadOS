#!/usr/bin/env python3
"""ThreadOS smoke test: the cursor, windows, controls, a ring 3
application, the taskbar.

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

# These positions were measured when the title bar was ten pixels tall.
# Everything under a window's bar has moved down by the difference, and
# what is centred in the bar by half of it - said once here, so that the
# next change of the bar moves the positions with it.
DOWN = TITLE_H - 10
MID = (TITLE_H - 10) // 2
from ptest import check, summary


def main():
    # --- Graphics mode (a separate boot: 'gfx' is one-way) ---
    print("--- graphics mode (screendump) ---")
    q3 = Qemu(KERNEL, INITRD, append=TEXT_MODE)
    try:
        q3.wait_for("ThreadOS shell")
        q3.type_line("gfx")
        time.sleep(1.0)
        sx, sy, at = q3.screen_pixels()
        check(f"graphics mode is on (the screen scaled {sx}x{sy})",
              sx >= 1 and sy >= 1)

        def near(px, want, tol=40):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        check("framebuffer: a blue background (fb 160,180)",
              near(at(160, 180), (0, 0, 255)))
        check("framebuffer: a red rectangle in the middle (fb 160,100)",
              near(at(160, 100), (255, 0, 0)))
        check("framebuffer: a green rectangle in the corner (fb 30,30)",
              near(at(30, 30), (0, 255, 0)))
        check("framebuffer: a yellow rectangle in the corner (fb 290,30)",
              near(at(290, 30), (255, 255, 0)))
    finally:
        q3.close()

    # --- The graphical desktop: the mouse cursor (a separate boot) ---
    print("--- the desktop: the mouse pointer ---")
    q4 = Qemu(KERNEL, INITRD)
    try:
        q4.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=40):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # The desktop runs at a high VESA resolution (640x480)
        gw, gh, _ = q4.screen_raw()
        check(f"the desktop in a high-resolution VESA mode ({gw}x{gh})",
              gw == 640 and gh == 480)

        # the mouse starts in the middle (160,100); move it to ~(50,40)
        q4.mouse_move(-110, -60)
        check("the pointer is drawn where the mouse is (white ~50,41)",
              q4.wait_pixel(50, 41, (255, 255, 255)))
        _, _, at = q4.screen_raw()
        check("the desktop background (teal) away from the pointer, the icons and the windows",
              near(at(350, 200), (0, 128, 160)))
        # move it somewhere else - the cursor follows
        q4.mouse_move(150, 80)
        check("the pointer follows the mouse (white ~200,121)",
              q4.wait_pixel(200, 121, (255, 255, 255)))
        check("the pointer's old place was cleaned up (teal ~50,41)",
              q4.wait_pixel(50, 41, (0, 128, 160)))

        # Text (an 8x8 font): the title bar with the caption "ThreadOS".
        # The bar is light grey; the glyph black -> a letter's cell has
        # both black and grey pixels (that is, it is a shape, not a solid
        # rectangle).
        _, _, at = q4.screen_raw()
        check("the title bar is drawn (light grey, empty space)",
              near(at(200, 4), (192, 192, 192)))
        cell = [at(2 + c, 1 + r) for r in range(8) for c in range(8)]
        blk = sum(1 for p in cell if near(p, (0, 0, 0)))
        gry = sum(1 for p in cell if near(p, (192, 192, 192)))
        check(f"text drawn in the 8x8 font (the 'P' glyph: {blk} black, "
              f"{gry} grey)", blk > 5 and gry > 5)
    finally:
        q4.close()

    # --- The desktop: windows and dragging with the mouse (a separate
# boot) ---
    print("--- the desktop: windows + drag ---")
    q5 = Qemu(KERNEL, INITRD)
    try:
        q5.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # The windows: win0 (40,60,120x70) and win1 (180,95). The mouse
# starts at (160,100).
        #
        # A title bar is now one of two colours: blue for the window
        # with the focus, a dim slate for the rest - so that WHICH
        # window is listening can be seen instead of remembered. At
        # boot nothing has the focus, so both of these are slate.
        # Past the name written on the bar, so that what is read is the
        # bar and not a letter of it
        GREY = (178, 178, 178)
        check("window 0: the title bar is drawn (115,64)",
              q5.wait_pixel(115, 64, GREY, 25))
        _, _, at = q5.screen_raw()
        check("window 0: a grey background (80,100)",
              near(at(80, 100), (224, 224, 224)))
        check("window 1: the title bar is drawn (240,99)",
              near(at(240, 99), GREY, 25))

        # Dragging window 0: the cursor (160,100) -> win0's bar (60,64),
        # press the left button, move (+80,+50), release. The window ->
        # (120,110).
        q5.mouse_move(-100, -36)
        q5.mouse_button(True)
        q5.mouse_move(80, 50)
        q5.mouse_button(False)
        # win0's new bar in the visible (unobscured) part: (150,114)
        # Dragging a window's bar focuses it, so from here that bar is
        # the blue one - which is itself the thing worth seeing
        # Dragging a window's bar gives it the focus, so from here that
        # bar is the accent blue - which is itself worth seeing
        check("window 0 was dragged to a new place (the bar ~200,118)",
              q5.wait_pixel(200, 118, (0, 0, 255), 30))
        _, _, at = q5.screen_raw()
        check("window 0's old place was cleaned up (teal ~60,90)",
              near(at(60, 90), (0, 128, 160)))
        check("window 1 was untouched during the drag (240,99)",
              near(at(240, 99), GREY, 25))
    finally:
        q5.close()

    # --- The desktop: clickable controls (an action button + closing) ---
    print("--- the desktop: controls (a click) ---")
    q6 = Qemu(KERNEL, INITRD)
    try:
        q6.wait_desktop()
        time.sleep(1.0)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # win0 (40,60,120x70): the [x] button in red in the bar's right corner
        check("the close button [x] (red, ~153,%d)" % (63 + MID),
              q6.wait_pixel(153, 63 + MID, (255, 0, 0)))
        _, _, at = q6.screen_raw()

        # The counter next to the 'Klik' button (the cell ~94,80). We click
# 3 times.
        cell_before = [at(94 + c, 80 + DOWN + r) for r in range(8) for c in range(8)]
        q6.mouse_goto(68, 84 + DOWN)            # the centre of the 'Klik' button
        for _ in range(3):
            q6.mouse_click()
        time.sleep(0.6)
        _, _, at = q6.screen_raw()
        cell_after = [at(94 + c, 80 + DOWN + r) for r in range(8) for c in range(8)]
        # The counter stands on the window's grey, so it is black -
        # white on that grey was drawn and could not be read
        white_after = sum(1 for p in cell_after if near(p, (0, 0, 0), 10))
        check("the action button answers a click (the counter changed a digit)",
              cell_before != cell_after and white_after > 0)

        # Closing a window: a click on window 1's [x] (the close box ~293,99)
        q6.mouse_goto(293, 99 + MID)
        q6.mouse_click()
        check("a click on [x] closes the window (win1's bar goes, the background is back)",
              q6.wait_pixel(220, 99 + MID, (0, 128, 160)))
        _, _, at = q6.screen_raw()
        # window 0 is still there (the click did not touch other windows)
        # Clicking its button gave window 0 the focus, so its bar is
        # the accent blue
        check("the other window stays open after the close (win0 bar 115,68)",
              near(at(115, 68), (0, 0, 255)))
    finally:
        q6.close()

    # --- A ring 3 application with its own window (drawing through
# syscalls) ---
    print("--- the desktop: a ring 3 application ---")
    q7 = Qemu(KERNEL, INITRD)
    try:
        q7.wait_desktop()
        time.sleep(0.8)
        q7.type_line("run guiapp")
        time.sleep(1.2)

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # guiapp -> slot 2 (slots 0/1 are the kernel's windows): the window
        # (94,54), the content area from (95,65). The RGB squares are
        # drawn through syscalls.
        check("the ring 3 application drew a red square (101,71)",
              q7.wait_pixel(101, 71 + DOWN, (255, 0, 0)))
        check("the ring 3 application drew a blue square (170,75)",
              q7.wait_pixel(170, 75 + DOWN, (0, 0, 255)))

        # Events: a click in the window's body reaches the application
        # (win_poll), which draws a marker in the accent colour at the place of the
        # click. The counter 'klikniec:' at (175,89) grows. We move the
        # cursor away so that it does not cover the marker in the
        # screenshot.
        cnt_before = [at(175 + c, 89 + DOWN + r) for r in range(8) for c in range(8)]
        q7.mouse_goto(150, 110 + DOWN)
        q7.mouse_click()
        q7.mouse_goto(60, 140)            # move the cursor away from the marker
        check("the click reached the application: its mark is in place (149,109)",
              q7.wait_pixel(149, 109 + DOWN, (0, 0, 255)))
        _, _, at = q7.screen_raw()
        cnt_after = [at(175 + c, 89 + DOWN + r) for r in range(8) for c in range(8)]
        check("the application counted the click (the counter changed a digit)",
              cnt_before != cnt_after)

        # The keyboard to the window with focus: the window is focused
        # (clicked above). After typing characters the field 'txt:'
        # (~135,103) shows the text.
        for k in ("a", "b", "c"):
            q7.sendkey(k)

        def txt_white():          # the typed text - black on the grey
            _, _, a = q7.screen_raw()
            return sum(1 for x in range(135, 159) for y in range(103 + DOWN, 111 + DOWN)
                       if near(a(x, y), (0, 0, 0), 10))

        appeared = False
        for _ in range(15):          # poll: the text appears after a few frames
            if txt_white() > 0:
                appeared = True
                break
            time.sleep(0.2)
        check("the keyboard reaches the window with the focus (the text appeared)",
              appeared)

        # A click on the desktop (the empty background) removes the focus, so
        # that 'kill' reaches the kernel's shell rather than the
        # application's window
        q7.mouse_goto(60, 140)
        q7.mouse_click()
        time.sleep(0.2)

        # Killing the application: the compositor cleans its window up. We
        # kill by name - the pid depends on how many threads started
        # earlier
        q7.type_line("kill guiapp")
        check("once the application is killed its window is cleaned up (170,75 teal)",
              q7.wait_pixel(170, 75 + DOWN, (0, 128, 160)))
    finally:
        q7.close()

    # --- The taskbar and the start menu (a separate boot) ---
    print("--- the desktop: the taskbar + the menu ---")
    q8 = Qemu(KERNEL, INITRD)
    try:
        q8.wait_desktop()

        def near(px, want, tol=45):
            return all(abs(px[i] - want[i]) <= tol for i in range(3))

        # The taskbar at the bottom (640x480 -> y=468); the 'Menu' button
# x2..42
        # The Menu button is raised now: its top row is the lit edge, so
        # the blue itself is a row lower
        # Past the word 'Menu' (six pixels in, four characters wide)
        check("the taskbar: the 'Menu' button (blue, 38,471)",
              q8.wait_pixel(38, 471, (0, 0, 255), 30))
        _, _, at = q8.screen_raw()
        # before the application starts, slot 2 is empty (the kernel window's
        # background)
        check("before the launch there is no application window (101,71 is not red)",
              not near(at(101, 71 + DOWN), (255, 0, 0)))

        # The menu entries are computed from MENU_ITEMS (see menu_y)
        q8.mouse_goto(10, 472)
        q8.mouse_click()
        check("a click on 'Menu' opens the menu (a grey entry)",
              q8.wait_pixel(70, menu_y("guiapp"), (224, 224, 224)))

        # A click on the 'guiapp' entry (entry0) -> starts the application
        q8.mouse_goto(40, menu_y("guiapp"))
        q8.mouse_click()
        check("a choice from the menu starts the application (a red square 101,71)",
              q8.wait_pixel(101, 71 + DOWN, (255, 0, 0)))
        _, _, at = q8.screen_raw()
        check("the menu closes after a choice (the entry goes)",
              not near(at(70, menu_y("guiapp")), (224, 224, 224)))

        # The window list on the taskbar: the Terminal/Files/guiapp buttons.
        # guiapp is on top (freshly started) - red at (101,71). A click on
        # the 'Terminal' button (slot0, j0, x46..96, y468) brings it to
        # the top and focuses it: guiapp's red gets covered by the
        # kernel's window.
        q8.mouse_goto(60, 473)
        q8.mouse_click()
        # Two pixels into the raised window's content (its corner is at
        # y=60): just under its bar, where guiapp's red square stood
        check("a click on the bar button brings the window to the top (the red one is covered)",
              q8.wait_pixel(101, 60 + CONTENT_DY + 2, (224, 224, 224), 15))
        # The focused window's button wears the accent colour. Its top
        # row carries nothing: the picture on it starts three pixels
        # down. The bar is eighteen tall now, so it begins at 462.
        check("the focused window is highlighted on the taskbar (60,464)",
              q8.wait_pixel(60, 464, (0, 0, 255), 30))
    finally:
        q8.close()

    return summary()


if __name__ == "__main__":
    sys.exit(main())
