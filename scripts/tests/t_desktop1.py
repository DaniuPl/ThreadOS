#!/usr/bin/env python3
"""ThreadOS smoke test: the cursor, the background and the bar, windows,
controls, a ring 3 application, the dock and the menu.

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
    print("--- the desktop: the mouse pointer, the background, the bar ---")
    q4 = Qemu(KERNEL, INITRD)
    try:
        q4.wait_desktop()
        time.sleep(1.0)

        # The desktop runs at a high VESA resolution (640x480)
        gw, gh, _ = q4.screen_raw()
        check(f"the desktop in a high-resolution VESA mode ({gw}x{gh})",
              gw == 640 and gh == 480)

        # the mouse starts in the middle (160,100); move it to ~(50,40)
        q4.mouse_move(-110, -60)
        check("the pointer is drawn where the mouse is (white ~50,41)",
              q4.wait_pixel(50, 41, (255, 255, 255)))
        # The background is the aurora, and it is the aurora to the
        # last bit: the test draws it again from the numbers in look.h
        # by the arithmetic kernel/aurora.c spells out
        _, _, at = q4.screen_raw()
        spots = [(350, 200), (600, 300), (500, 60), (20, 400), (330, 330)]
        exact = [(x, y) for x, y in spots if at(x, y) == aurora_px(x, y)]
        check("the background is the aurora, pixel for pixel (%d of %d "
              "places exact)" % (len(exact), len(spots)),
              len(exact) == len(spots))
        # move it somewhere else - the cursor follows
        q4.mouse_move(150, 80)
        check("the pointer follows the mouse (white ~200,121)",
              q4.wait_pixel(200, 121, (255, 255, 255)))
        check("the pointer's old place was cleaned up (the aurora at 50,41)",
              q4.wait_background(50, 41, tol=0))

        # The bar: glass - the aurora under it blurred and tinted, which
        # the test works out as well. Away from what is written on it,
        # the bar is that and nothing else.
        _, _, at = q4.screen_raw()
        glass = bar_glass()
        xs = [x for x in range(130, 330, 7)]
        same = sum(1 for x in xs for y in (2, 12, 20)
                   if near(at(x, y), glass[y][x], 1))
        check("the bar is glass: the blurred aurora under the tint (%d of "
              "%d pixels)" % (same, 3 * len(xs)), same == 3 * len(xs))
        name = q4.read_ui(BAR_NAME_X, ui_font_baseline_in(BAR_PX, 0, BAR_H),
                          width=90, px=BAR_PX, face="bold", under=on_bar)
        check("the bar says the system's name in bold TrueType ('%s')" % name,
              name == "ThreadOS")
    finally:
        q4.close()

    # --- The desktop: windows and dragging with the mouse (a separate
    # boot) ---
    print("--- the desktop: windows + drag ---")
    q5 = Qemu(KERNEL, INITRD)
    try:
        q5.wait_desktop()
        time.sleep(1.0)
        dark = theme("dark")

        check("the desktop has no placeholder windows", q5.dock_windows() == 0)
        q5.launch_dock(4)
        time.sleep(3)
        check("a real application opens in the first cascade slot",
              q5.wait_focus(94, 54))

        # Drag the app by its title bar, away from the lights.
        q5.mouse_goto(160, 58)
        q5.mouse_button(True)
        q5.mouse_move(80, 50)
        q5.mouse_button(False)
        check("dragging the application window moves it and keeps focus",
              q5.wait_focus(174, 104))
        _, _, at = q5.screen_raw()
        check("the title bar shows the focused colour",
              near(at(244, 107), dark["title_bg"], 3))
    finally:
        q5.close()

    # --- The desktop: closing an application window -------------------
    print("--- the desktop: closing an application ---")
    q6 = Qemu(KERNEL, INITRD)
    try:
        q6.wait_desktop()
        time.sleep(1.0)
        q6.launch_dock(4)
        time.sleep(3)
        check("the application opened", q6.dock_windows() == 1)
        q6.click(*light_xy(94, 54, 0))
        q6.mouse_goto(300, 300)
        time.sleep(3)
        check("the close light closes the application window",
              q6.dock_windows() == 0)
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

        # guiapp opens in its first cascade position, at (94,54). The RGB
        # squares are drawn through syscalls.
        check("the ring 3 application drew a red square",
              q7.wait_pixel(101, 71 + DOWN, (255, 0, 0)))
        check("the ring 3 application drew a blue square",
              q7.wait_pixel(170, 75 + DOWN, (0, 0, 255)))

        # Events: a click in the window's body reaches the application
        # (win_poll), which draws a marker in the accent colour at the place of the
        # click. The counter 'klikniec:' at (175,89) grows. We move the
        # cursor away so that it does not cover the marker in the
        # screenshot.
        _, _, at = q7.screen_raw()
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
        q7.mouse_goto(60, 400)
        q7.mouse_click()
        time.sleep(0.2)

        # Killing the application: the compositor cleans its window up. We
        # kill by name - the pid depends on how many threads started
        # earlier
        q7.type_line("kill guiapp")
        # (200,75) is inside guiapp's window.
        check("once the application is killed its window is cleaned up "
              "(the aurora at 200,75)", q7.wait_background(200, 75, tol=0))
    finally:
        q7.close()

    # --- The dock and the launch menu (a separate boot) ---
    print("--- the desktop: the dock + the menu ---")
    q8 = Qemu(KERNEL, INITRD)
    try:
        q8.wait_desktop()
        time.sleep(1.0)
        _, _, at = q8.screen_raw()
        # before the application starts, the first cascade slot is empty
        check("before the launch there is no application window (101,71 is not red)",
              not near(at(101, 71 + DOWN), (255, 0, 0)))

        # The menu hangs from the mark at the left end of the bar; its
        # entries are the programs' names, in TrueType
        def entry(name):
            top = menu_y(name) - MENU_ROW // 2
            return q8.read_ui(MENU_X0 + 10 + MENU_ICON + 10,
                              ui_font_baseline_in(MENU_PX, top, MENU_ROW),
                              width=100, px=MENU_PX)
        q8.open_menu()
        q8.mouse_goto(300, 300)
        time.sleep(0.6)
        got = entry("guiapp"), entry("gclock"), entry("gmikser")
        check("a click on the mark opens the menu (it reads %s)" % (got,),
              got == ("App", "Clock", "Mixer"))

        # A click on the 'guiapp' entry -> starts the application
        q8.click(MENU_X0 + 60, menu_y("guiapp"))
        q8.mouse_goto(300, 300)
        check("a choice from the menu starts the application (a red square 101,71)",
              q8.wait_pixel(101, 71 + DOWN, (255, 0, 0)))
        check("the menu closes after a choice (its entry is gone)",
              entry("guiapp") == "")

        # Opening a second program lets the first window's dock tile
        # bring the original application back to the top.
        q8.launch_menu("gclock")
        time.sleep(3)
        q8.click(*dock_window(0, 2))
        q8.mouse_goto(300, 300)
        check("a click on a window's tile brings the window to the top (the red one is covered)",
              q8.wait_pixel(101, 71 + DOWN, (255, 0, 0)))
        check("and gives it the focus", q8.wait_focus(94, 54))
        # The window that is listening has its dot in the accent colour
        g = dock_geom(n_w=2)
        tx, ty = g["tiles"][DOCK_N]
        dot = (tx + g["tile"] // 2, ty + g["tile"] + 1 + DOCK_DOT // 2)
        dot2 = (g["tiles"][DOCK_N + 1][0] + g["tile"] // 2, dot[1])
        _, _, at = q8.screen_raw()
        check("the focused window's dot in the dock is the accent (%s)"
              % (at(*dot),), near(at(*dot), theme("dark")["accent"], 50))
        check("and another window's dot is not (%s)" % (at(*dot2),),
              not near(at(*dot2), theme("dark")["accent"], 50))
    finally:
        q8.close()

    return summary()


if __name__ == "__main__":
    sys.exit(main())
