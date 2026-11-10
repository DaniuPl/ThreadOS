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
  tiles leave the dock), and the lock that is left says it is a log in

  Restart really restarts the machine and Shut Down really switches it
  off - QEMU runs with -no-reboot, so either way it ends, and the
  serial port says which of the two it was

Run on its own or through scripts/run_tests.py.
"""

import os
import re
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


def _vol_frames():
    """How many frames the loudness card stays: what kernel/desktop.c
    sets vol_until to, past the frame the key came in on"""
    full = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                        "..", "kernel", "desktop.c")
    with open(full) as fh:
        src = fh.read()
    hz = int(re.search(r"#define\s+FRAME_HZ\s+(\d+)", src).group(1))
    expr = re.search(r"vol_until = frames \+ ([^;]+);", src).group(1)
    return eval(expr.replace("/", "//"), {}, {"FRAME_HZ": hz})


VOL_FRAMES = _vol_frames()


def entry(q, name):
    """An entry read the way a person reads a menu - with the pointer on
    it: then it stands on the accent, one colour, instead of on glass
    that is whatever lies under the menu, blurred. (Where the menu hangs
    is ptest's menu_top(): on this screen higher than MENU_Y.)"""
    y = session_y(name)
    q.mouse_goto(MENU_X0 + MENU_W - 12, y)
    time.sleep(0.6)
    top = y - SESS_ROW // 2
    return q.read_ui(MENU_X0 + 10 + MENU_ICON + 10,
                     ui_font_baseline_in(MENU_PX, top, SESS_ROW),
                     width=120, px=MENU_PX, fg=DARK["accent_text"])


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


# Beside the mark at the left end of the bar, inside the rounded
# rectangle that lights it while the menu hangs from it (draw_bar() in
# kernel/desktop.c) and clear of the mark and of the name
MARK_SPOTS = [(x, y) for x in (6, 7, BAR_LOGO_X + BAR_LOGO_D + 3)
              for y in (BAR_H // 2 - 2, BAR_H // 2, BAR_H // 2 + 2)]


def mark(at):
    return [at(x, y) for x, y in MARK_SPOTS]


def lit(before, now):
    """Is 'now' the bar of 'before' with the mark lit: the bar's text
    colour laid over it at 40 of 255?"""
    return all(near(n, gfx_mix(DARK["bar_text"], b, 40), 3)
               for b, n in zip(before, now))


def lock_says(q):
    fy = SCREEN_H * 2 // 3 - LOCK_FIELD_H
    return q.find_ui(80, SCREEN_W - 80, fy + LOCK_FIELD_H + 22, px=UI_PX,
                     fg=(0xD8, 0xD8, 0xDC), under=under)


def lock_profile(q):
    """The profile's name over the fields of the logged-out lock - white,
    on the darkened aurora"""
    w, h, at = q.screen_raw()
    fy = h * 2 // 3 - LOCK_FIELD_H
    return q.find_ui(80, w - 80, fy - 70, px=15, face="bold",
                     fg=(0xFF, 0xFF, 0xFF), under=under, screen=(w, h, at))


def typed(q, text):
    for ch in text:
        q.sendkey(Qemu.KEYNAMES.get(ch, ch))
        time.sleep(0.12)


def desktop_unlocked(q):
    return lock_says(q) == ""


def session(slog, disk):
    # (a disk, so that the profile made at the first start is saved, and
    # the lock after a log out has its name to show)
    q = Qemu(KERNEL, INITRD, disk, serial=slog)
    try:
        check("the desktop came up", q.wait_desktop(timeout=40))
        q.mouse_home()
        q.open_menu()
        q.mouse_goto(400, 300)
        time.sleep(0.6)
        got = [entry(q, n) for n in SESSION_ITEMS]
        q.mouse_goto(400, 300)
        time.sleep(0.6)
        scr = q.screen_raw()
        check("under the programs, the system's entries (%s)" % (got,),
              got == ["About ThreadOS", "Lock Screen", "Log Out...",
                      "Restart...", "Shut Down..."])
        _, _, at = scr
        line = at(MENU_X0 + MENU_W // 2, menu_top() + MENU_PAD +
                  len(MENU_ITEMS) * MENU_ROW + MENU_SEP // 2)
        below = at(MENU_X0 + MENU_W // 2, menu_top() + MENU_PAD +
                   len(MENU_ITEMS) * MENU_ROW + 1)
        check("with a line between them and the programs (%s on %s)"
              % (line, below), line != below)
        q.mouse_goto(MENU_X0 + 60, session_y("Restart..."))
        time.sleep(0.6)
        _, _, at = q.screen_raw()
        check("the entry under the pointer answers on the accent",
              near(at(MENU_X0 + 8, session_y("Restart...")), DARK["accent"], 6))
        q.click_desktop()

        # The keys: the Windows key opens the menu with its first entry
        # chosen, the arrows choose, Enter does, Esc closes. On this
        # screen the menu is too long to hang at MENU_Y and is moved up
        # to the bar - it is still the bar's menu: the mark is lit, and
        # the key and the mark close it again.
        q.mouse_goto(300, 1)              # the bar shows, the mark unlit
        time.sleep(1)
        _, _, at = q.screen_raw()
        unlit = mark(at)
        q.mouse_goto(400, 300)
        time.sleep(0.5)
        q.sendkey("meta_l")
        time.sleep(1)
        _, _, at = q.screen_raw()
        check("the Windows key opens the menu, its first entry chosen",
              near(at(MENU_X0 + 8, menu_y(MENU_ITEMS[0])), DARK["accent"], 6))
        now = mark(at)
        check("and the mark on the bar is lit while the menu hangs from it "
              "(%s -> %s)" % (unlit[0], now[0]), lit(unlit, now))
        q.sendkey("meta_l")
        time.sleep(1)
        _, _, at = q.screen_raw()
        check("the Windows key again closes it (the first entry %s)"
              % (at(MENU_X0 + 8, menu_y(MENU_ITEMS[0])),),
              not near(at(MENU_X0 + 8, menu_y(MENU_ITEMS[0])),
                       DARK["accent"], 6) and
              not near(at(MENU_X0 + 8, menu_y(MENU_ITEMS[1])),
                       DARK["accent"], 6))
        # One click on the mark, and the menu is shown to be there before
        # the second: two clicks that both went nowhere would leave the
        # same screen as an opening and a closing
        q.open_menu()
        q.mouse_goto(MENU_X0 + 60, menu_y(MENU_ITEMS[1]))
        end = time.time() + 10
        while True:
            _, _, at = q.screen_raw()
            under = at(MENU_X0 + 8, menu_y(MENU_ITEMS[1]))
            if near(under, DARK["accent"], 6) or time.time() > end:
                break
            time.sleep(0.3)
        check("a click on the mark opens the menu (the entry under the "
              "pointer answers: %s)" % (under,),
              near(under, DARK["accent"], 6))
        q.open_menu()
        q.mouse_goto(MENU_X0 + 60, menu_y(MENU_ITEMS[1]))
        time.sleep(1)
        _, _, at = q.screen_raw()
        check("and so does a second click on the mark (nothing answers "
              "under the pointer: %s)" % (at(MENU_X0 + 8,
                                              menu_y(MENU_ITEMS[1])),),
              not near(at(MENU_X0 + 8, menu_y(MENU_ITEMS[1])),
                       DARK["accent"], 6))
        q.mouse_goto(400, 300)
        time.sleep(0.5)
        q.sendkey("meta_l")
        time.sleep(1)
        q.sendkey("down")
        q.sendkey("down")
        time.sleep(0.8)
        _, _, at = q.screen_raw()
        check("Down twice chooses the third ('%s')" % MENU_ITEMS[2],
              near(at(MENU_X0 + 8, menu_y(MENU_ITEMS[2])), DARK["accent"], 6)
              and not near(at(MENU_X0 + 8, menu_y(MENU_ITEMS[0])),
                           DARK["accent"], 6))
        before = q.dock_windows()
        q.sendkey("ret")
        time.sleep(4)
        check("and Enter starts it (%d -> %d windows)"
              % (before, q.dock_windows()), q.dock_windows() == before + 1)
        q.click_desktop()
        time.sleep(0.5)
        q.mouse_goto(400, 300)
        time.sleep(0.5)
        q.sendkey("meta_l")
        time.sleep(0.5)
        q.sendkey("up")
        time.sleep(0.8)
        _, _, at = q.screen_raw()
        check("Up from the first goes round to the last ('Shut Down...')",
              near(at(MENU_X0 + 8, session_y("Shut Down...")),
                   DARK["accent"], 6))
        q.sendkey("esc")
        time.sleep(0.8)
        _, _, at = q.screen_raw()
        check("and Esc closes it, having done nothing",
              not near(at(MENU_X0 + 8, session_y("Shut Down...")),
                       DARK["accent"], 6) and q.proc.poll() is None)
        # (the program started from the keys goes again: it is the one
        # window in the dock)
        q.click(*dock_window(0, 1))
        time.sleep(0.5)
        q.sendkey("alt-f4")
        time.sleep(2)

        # The loudness keys a laptop has on Fn: a card above the dock
        # says how loud it is now, and the setting keeps it
        def loudness():
            g = dock_geom()
            x = (SCREEN_W - VOL_W) // 2
            y = g["y"] - VOL_DY - VOL_H
            return q.read_ui(x + VOL_BAR_X + VOL_BAR_W + 12,
                             ui_font_baseline_in(MENU_PX, y, VOL_H), 60,
                             px=MENU_PX, fg=DARK["menu_text"])
        q.mouse_goto(400, 300)
        fps0 = q.frame_rate()
        q.sendkey("volumeup")
        q.sendkey("volumeup")
        time.sleep(0.8)
        said = loudness()
        check("Volume Up twice: the card says 90%% ('%s')" % said,
              said == "90%")
        check("and the setting keeps it",
              "volume = 90" in q.command("set", "slower", timeout=10))
        q.sendkey("audiomute")
        time.sleep(0.8)
        said = loudness()
        check("Mute: the card says Off ('%s')" % said, said == "Off")
        q.sendkey("volumedown")
        pressed = time.time()
        time.sleep(0.8)
        said = loudness()
        check("and Volume Down brings the sound back, a step lower ('%s')"
              % said, said == "85%")
        # The moment is counted in the desktop's frames (vol_until), and
        # an emulated machine draws fewer than fifty a second - fewer
        # still while the card is up, its glass blurred afresh every
        # frame: so the card is looked for until it is gone, and how
        # long it stayed is held to those frames at the slowest of the
        # rates the desktop said it drew before, while it was up and
        # after - twice as long, for a rate that dips between the
        # readings, and three seconds for the typing and the reading.
        # (Gone means gone on two readings in a row, so that one taken
        # while the card is redrawn cannot pass for it.)
        fps1 = q.frame_rate()
        gone = blank = None
        while time.time() < pressed + 60:
            if loudness() != "":
                blank = None
            elif blank is None:
                blank = time.time() - pressed
            else:
                gone = blank
                break
            time.sleep(0.3)
        rates = [r for r in (fps0, fps1, q.frame_rate()) if r]
        fps = min(rates) if rates else None
        bound = 2 * VOL_FRAMES / fps + 3 if fps else 0
        check("the card goes after a moment (%s, by %.1f s: %d frames at "
              "%s a second of %s)" % ("%.1f s" % gone if gone else
                                      "still there", bound, VOL_FRAMES, fps,
                                      rates),
              gone is not None and gone <= bound)

        # About: what the machine is, said on a card - against what the
        # test knows it started (the 64-bit kernel on one processor, a
        # 640x480 screen, the memory QEMU was given)
        pick(q, "About ThreadOS")
        ay = (SCREEN_H - ABOUT_H) // 2
        tx = AX + ASK_PAD + 40 + 14
        scr = q.screen_raw()
        about = [q.read_ui(tx, ay + ASK_TITLE_Y, 260, px=15, face="bold",
                           fg=DARK["app_text"], screen=scr)]
        about += [q.read_ui(tx, ay + ABOUT_LINE_Y + i * ABOUT_LINE_DY, 260,
                            px=UI_PX, fg=DARK["app_dim"], screen=scr)
                  for i in range(4)]
        check("About ThreadOS says what the machine is (%s)" % (about,),
              about[0] == "ThreadOS" and
              about[1] == "64-bit kernel, 1 processor" and
              re.match(r"Memory: \d+ MiB, \d+ MiB free$", about[2]) and
              about[3] == "Screen: 640x480 at 100%" and
              about[4].startswith("Up for "))
        q.sendkey("ret")
        time.sleep(1)
        check("and Enter closes it",
              q.read_ui(tx, ay + ASK_TITLE_Y, 260, px=15, face="bold",
                        fg=DARK["app_text"]) == "" and desktop_unlocked(q))

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
                  question(q)[0] == "" and desktop_unlocked(q) and
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
        # The kernel's shell echoes every key it gets on the serial port,
        # so a key that reached it past the card would be there (the
        # kernel's own messages, lines in brackets, are not the shell's)
        def echoed(since):
            return "".join(ln for ln in q.serial()[since:].split("\n")
                           if not ln.lstrip().startswith("["))
        seen = len(q.serial())
        typed(q, "x")
        q.sendkey("esc")
        time.sleep(1)
        stray = echoed(seen)
        check("Esc is Cancel, and the key before it went nowhere (the shell "
              "echoed %r)" % stray,
              question(q)[0] == "" and q.proc.poll() is None and
              "x" not in stray)
        # ... where with no card the same key does arrive, at once
        seen = len(q.serial())
        typed(q, "x")
        end = time.time() + 5
        while "x" not in echoed(seen) and time.time() < end:
            time.sleep(0.2)
        got = echoed(seen)
        q.sendkey("backspace")
        check("(with no card the shell gets that key and echoes it: %r)"
              % got, "x" in got)

        # Lock Screen locks
        pick(q, "Lock Screen")
        said = lock_says(q)
        check("Lock Screen locks ('%s')" % said,
              "to unlock" in said)
        typed(q, "thread\n")
        time.sleep(2)
        q.mouse_goto(600, 300)
        time.sleep(1)
        check("and the password opens it", desktop_unlocked(q))

        # Log Out: the programs end and the profile locks
        q.click_desktop()
        q.type_line("run gclock")
        time.sleep(3)
        q.mouse_goto(600, 300)
        time.sleep(1)
        check("before: a program is open (1 window in the dock)",
              q.dock_windows() == 1)
        pick(q, "Log Out...")
        q.sendkey("ret")
        time.sleep(2)
        said = lock_says(q)
        check("Enter logs out, and the lock says it is a log in ('%s')"
              % said, "signs in" in said and not desktop_unlocked(q))
        shown = lock_profile(q)
        check("the logged-out screen shows the saved profile name ('%s')"
              % shown, shown == "Test Admin")
        typed(q, "thread\n")
        time.sleep(2)
        q.mouse_goto(600, 300)
        time.sleep(1)
        check("logged in again: the program was closed (no windows in the "
              "dock)",
              q.dock_windows() == 0 and desktop_unlocked(q))

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
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    try:
        session(slog, disk)
        shutdown(slog2)
    finally:
        for f in (slog, slog2, disk):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


sys.exit(main())
