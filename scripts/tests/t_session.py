#!/usr/bin/env python3
"""ThreadOS smoke test: the menu's system part - locking, logging out,
restarting and shutting down.

Under a line at the bottom of the menu that starts the programs: Lock
Screen, Log Out..., Restart... and Shut Down... The three with dots
end something, and what is asserted is what that promises:

  the entries are there, read off the screen, and the one under the
  pointer answers on the accent

  each of the three asks first, in words read off the screen - and
  Cancel, Esc, and a click that misses the buttons all leave the
  machine running with everything on it

  Lock Screen locks; Log Out closes the programs that were open (their
  tiles leave the dock), leaves the kernel's own windows, and the lock
  that is left says it is a log in

  Restart really restarts the machine and Shut Down really switches it
  off - QEMU runs with -no-reboot, so either way it ends, and the
  serial port says which of the two it was

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

DARK = theme("dark")
AX, AY = (SCREEN_W - ASK_W) // 2, (SCREEN_H - ASK_H) // 2
BTN_Y = AY + ASK_H - ASK_PAD - ASK_BTN_H
DO_X = AX + ASK_W - ASK_PAD - ASK_BTN_W
NO_X = DO_X - ASK_BTN_GAP - ASK_BTN_W


def entry(q, name, screen=None):
    top = session_y(name) - MENU_ROW // 2
    return q.read_ui(MENU_X0 + 10 + MENU_ICON + 10,
                     ui_font_baseline_in(MENU_PX, top, MENU_ROW),
                     width=120, px=MENU_PX, fg=DARK["menu_text"],
                     screen=screen)


def pick(q, name):
    q.open_menu()
    q.click(MENU_X0 + 60, session_y(name))
    q.mouse_goto(SCREEN_W // 2, AY - 40)
    time.sleep(1)


def question(q):
    """The question on the card, its button, and Cancel - '' when there
    is no card"""
    scr = q.screen_raw()
    title = q.read_ui(AX + ASK_PAD + 40 + 14, AY + ASK_TITLE_Y, 260, px=15,
                      face="bold", fg=DARK["app_text"], screen=scr)
    base = ui_font_baseline_in(UI_PX, BTN_Y, ASK_BTN_H)
    act = q.find_ui(DO_X + 2, DO_X + ASK_BTN_W - 2, base, px=UI_PX,
                    face="bold", fg=DARK["accent_text"], screen=scr)
    no = q.find_ui(NO_X + 2, NO_X + ASK_BTN_W - 2, base, px=UI_PX,
                   fg=DARK["app_text"], screen=scr)
    return title, act, no


def under(x, y):
    return gfx_mix((0, 0, 0), aurora_px(x, y), 90)


def lock_says(q):
    fy = SCREEN_H * 2 // 3 - LOCK_FIELD_H
    return q.find_ui(80, SCREEN_W - 80, fy + LOCK_FIELD_H + 22, px=UI_PX,
                     fg=(0xD8, 0xD8, 0xDC), under=under)


def typed(q, text):
    for ch in text:
        q.sendkey(Qemu.KEYNAMES.get(ch, ch))
        time.sleep(0.12)


def kernel_window(q):
    return q.window_title(40, 60, 122) != ""


def session(slog):
    q = Qemu(KERNEL, INITRD, serial=slog)
    try:
        check("the desktop came up", q.wait_desktop(timeout=40))
        q.mouse_home()
        q.open_menu()
        q.mouse_goto(400, 300)
        time.sleep(0.6)
        scr = q.screen_raw()
        got = [entry(q, n, scr) for n in SESSION_ITEMS]
        check("under the programs, the system's entries (%s)" % (got,),
              got == ["Lock Screen", "Log Out...", "Restart...",
                      "Shut Down..."])
        _, _, at = scr
        line = at(MENU_X0 + MENU_W // 2,
                  MENU_Y + MENU_PAD + len(MENU_ITEMS) * MENU_ROW + MENU_SEP // 2)
        below = at(MENU_X0 + MENU_W // 2,
                   MENU_Y + MENU_PAD + len(MENU_ITEMS) * MENU_ROW + 1)
        check("with a line between them and the programs (%s on %s)"
              % (line, below), line != below)
        q.mouse_goto(MENU_X0 + 60, session_y("Restart..."))
        time.sleep(0.6)
        _, _, at = q.screen_raw()
        check("the entry under the pointer answers on the accent",
              near(at(MENU_X0 + 8, session_y("Restart...")), DARK["accent"], 6))
        q.click_desktop()

        # Each of the three asks first
        for name, want, act in (("Log Out...", "Log out?", "Log Out"),
                                ("Restart...", "Restart the computer?",
                                 "Restart"),
                                ("Shut Down...", "Shut down the computer?",
                                 "Shut Down")):
            pick(q, name)
            got = question(q)
            check("'%s' asks first: %s" % (name, got),
                  got == (want, act, "Cancel"))
            q.click(NO_X + ASK_BTN_W // 2, BTN_Y + ASK_BTN_H // 2)
            q.mouse_goto(SCREEN_W // 2, AY - 40)
            time.sleep(1)
            check("and Cancel leaves everything as it was ('%s' gone, the "
                  "window there)" % question(q)[0],
                  question(q)[0] == "" and kernel_window(q) and
                  q.proc.poll() is None)

        # Neither Esc nor a click beside the buttons is a yes
        pick(q, "Shut Down...")
        q.click(AX + 30, AY + ASK_H - 20)
        q.click(40, 420)
        q.mouse_goto(SCREEN_W // 2, AY - 40)
        time.sleep(1)
        check("a click beside the buttons answers nothing ('%s' still asked)"
              % question(q)[0],
              question(q)[0] == "Shut down the computer?" and
              q.proc.poll() is None)
        seen = len(q.serial())
        typed(q, "x")
        q.sendkey("esc")
        time.sleep(1)
        check("Esc is Cancel, and the key before it went nowhere",
              question(q)[0] == "" and q.proc.poll() is None and
              "unknown command" not in q.serial()[seen:])

        # Lock Screen locks
        pick(q, "Lock Screen")
        said = lock_says(q)
        check("Lock Screen locks ('%s')" % said,
              not kernel_window(q) and "to unlock" in said)
        typed(q, "thread\n")
        time.sleep(2)
        q.mouse_goto(600, 300)
        time.sleep(1)
        check("and the password opens it", kernel_window(q))

        # Log Out: the programs end, the kernel's windows stay
        q.click_desktop()
        q.type_line("run gclock")
        time.sleep(3)
        q.mouse_goto(600, 300)
        time.sleep(1)
        check("before: a program is open (3 windows in the dock)",
              q.dock_windows() == 3)
        pick(q, "Log Out...")
        q.sendkey("ret")
        time.sleep(2)
        said = lock_says(q)
        check("Enter logs out, and the lock says it is a log in ('%s')"
              % said, "to log in" in said and not kernel_window(q))
        typed(q, "thread\n")
        time.sleep(2)
        q.mouse_goto(600, 300)
        time.sleep(1)
        check("logged in again: the program was closed (2 windows in the "
              "dock), the kernel's own windows stayed",
              q.dock_windows() == 2 and kernel_window(q))

        # Restart, for real
        pick(q, "Restart...")
        seen = len(q.serial())
        # The press is the click here, as everywhere on this desktop -
        # and after it there is no machine left to release the button on
        q.mouse_goto(DO_X + ASK_BTN_W // 2, BTN_Y + ASK_BTN_H // 2)
        time.sleep(0.3)
        q.mouse_button(True)
        ended = q.wait_exit(timeout=20)
        check("Restart restarts the machine (QEMU ended: %s)" % ended,
              ended and "Restarting" in q.serial()[seen:])
    finally:
        q.close()


def shutdown(slog):
    q = Qemu(KERNEL, INITRD, serial=slog)
    try:
        check("the desktop came up again", q.wait_desktop(timeout=40))
        q.mouse_home()
        pick(q, "Shut Down...")
        seen = len(q.serial())
        q.sendkey("ret")
        ended = q.wait_exit(timeout=20)
        check("Shut Down switches it off (QEMU ended: %s)" % ended,
              ended and "Powering off" in q.serial()[seen:])
    finally:
        q.close()


def main():
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")
    try:
        session(slog)
        shutdown(slog2)
    finally:
        for f in (slog, slog2):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


sys.exit(main())
