#!/usr/bin/env python3
"""ProtonOS smoke test: Alt+F4 and the menu under the right button.

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
  pointer is. It is the same list as the Menu button's, because a
  second list would be a second thing to learn - only it comes to the
  hand instead of the hand going to the corner. On a window the right
  button opens nothing (a window's own menu is the window's business)
  and closes a menu that was open.

The assertions are about what a person would notice: the program
started from the menu really runs (it draws), the menu stands at the
pointer and not over the Menu button, it never hangs off the screen or
over the taskbar, Alt+F4 ends the program with its own exit code, and
the window goes from the screen AND from the taskbar.

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

TB_H = 18
TBL_X0 = 46
CLOCK_W = 8 * 8 + 6
MENU_ROW = 16
MENU_W = 110
MENU_N = 4

# The cascade: slot i sits at (70+12i, 30+12i), and applications start
# at slot 2 because the kernel's own two windows hold 0 and 1.
WX, WY = 94, 54

# gsettings asks for a 360x210 content buffer
P_APP = (400, 200)          # inside it, past both kernel windows


def near(px, v):
    """A grey of exactly this shade? The six-bit palette entries land
    a shade apart from each other on the screen (56 -> 227, 52 -> 210),
    so 'grey' alone would not tell a window from the taskbar."""
    return all(abs(c - v) <= 6 for c in px)


def is_win(px):
    return near(px, 227)    # COL_WIN, the window / menu background


def is_bar(px):
    return near(px, 210)    # COL_BAR, the taskbar


def is_accent(px):
    r, g, b = px            # COL_TITLE, the blue of "this one is listening"
    return b > 120 and r < 90 and g < 90


def bar_buttons(at, w, y):
    """The spans of the taskbar buttons: everything on the bar row that
    is not the bar's own grey."""
    out, start = [], None
    for x in range(TBL_X0, w - CLOCK_W - 2):
        on_bar = is_bar(at(x, y))
        if not on_bar and start is None:
            start = x
        elif on_bar and start is not None:
            out.append((start, x - 1))
            start = None
    if start is not None:
        out.append((start, w - CLOCK_W - 3))
    return out


def sixty_four():
    """The same two gestures on the 64-bit kernel. It is worth a boot of
    its own, because the road the keys travel is NOT shared: long mode
    has a keyboard driver of its own, and Alt+F4 had to be let through
    it separately. The compositor above is the same code, so here we ask
    only the one question that differs - do the keys arrive."""
    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("POMINIETE (64): brak obrazu 64-bitowego (make kernel64)")
        return
    if shutil.which("grub-mkrescue") is None or \
       shutil.which("qemu-system-x86_64") is None:
        print("POMINIETE (64): brak grub-mkrescue lub qemu-system-x86_64")
        return

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)

    try:
        os.makedirs(d + "/boot/grub")
        shutil.copy(KERNEL64, d + "/boot/protonos64.bin")
        mod = ""
        if INITRD64 and os.path.exists(INITRD64):
            shutil.copy(INITRD64, d + "/boot/initrd64.img")
            mod = "    module2 /boot/initrd64.img initrd64\n"
        with open(d + "/boot/grub/grub.cfg", "w") as fh:
            fh.write("insmod all_video\nset gfxpayload=keep\n"
                     "set timeout=0\nset default=0\n"
                     'menuentry "ProtonOS 64" {\n'
                     "    multiboot2 /boot/protonos64.bin\n" + mod + "}\n")
        made = subprocess.run(["grub-mkrescue", "-o", iso, d],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
        check("plyta z jadrem 64-bitowym buduje sie", made)
        if not made:
            return

        q = Qemu(KERNEL, INITRD, disk=disk, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=60)
            q.type_line("pulpit")
            q.wait_serial("Pulpit startuje", timeout=30)
            time.sleep(4)
            q.mouse_home()

            q.mouse_goto(24, 72)           # the 'Zegar' icon
            q.mouse_click()
            time.sleep(4)
            q.mouse_goto(600, 400)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            row = h - TB_H + 2
            opened = len(bar_buttons(at, w, row))
            check("(64) zegar wstal z ikony (%d przyciskow na pasku)"
                  % opened, opened == 3)

            # Closing a window is not instant, and how long it takes
            # is not ours to guess: with the whole suite running the
            # same three seconds are sometimes not enough. So we look
            # until the button is gone, and only then give up.
            q.sendkey("alt-f4")
            left = 3
            for _ in range(10):
                time.sleep(1.5)
                w, h, at = q.screen_raw()
                left = len(bar_buttons(at, w, row))
                if left == 2:
                    break
            check("(64) Alt+F4 dochodzi przez sterownik long mode i "
                  "zamyka okno (%d przyciskow)" % left, left == 2)

            q.mouse_goto(500, 300)
            time.sleep(0.5)
            q.mouse_right_click()
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("(64) prawy przycisk otwiera menu pod kursorem",
                  is_win(at(600, 300 + 2 * MENU_ROW + 8)) and
                  is_accent(at(600, 308)))
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
            check("pulpit wstal", q.wait_desktop(timeout=40))
            q.mouse_home()

            # --- Alt+F4 -------------------------------------------------
            q.mouse_goto(24, 272)          # the "Ustaw" icon (gsettings)
            q.mouse_click()
            time.sleep(4)
            q.mouse_goto(600, 400)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            row = h - TB_H + 2
            check("ustawienia stoja na ekranie", is_win(at(*P_APP)))
            before = len(bar_buttons(at, w, row))
            check("i maja swoj przycisk na pasku (%d z oknami jadra)"
                  % before, before == 3)

            q.sendkey("alt-f4")
            time.sleep(3)
            w, h, at = q.screen_raw()

            log = q.serial()
            # Not dropped - ASKED. A window pulled out from under a
            # process leaves it running; this one ended itself, which
            # is what its own exit code says.
            check("Alt+F4 poprosil program, a ten zakonczyl sie sam",
                  "proces 'gsettings' zakonczyl sie (kod 0)" in log)
            check("okno zniknelo z ekranu", not is_win(at(*P_APP)))
            after = len(bar_buttons(at, w, row))
            check("i zniknelo z paska zadan (%d -> %d przyciskow)"
                  % (before, after), after == before - 1)

            # Alt+F4 with nothing focused closes nothing - the kernel's
            # own windows are still there
            q.sendkey("alt-f4")
            time.sleep(2)
            w, h, at = q.screen_raw()
            check("Alt+F4 bez ogniska nie zamyka niczego - okna jadra "
                  "stoja gdzie staly", len(bar_buttons(at, w, row)) == after)

            # --- two clicks on the title bar ----------------------------
            # Every system does this, and a person who has used one
            # tries it here in the first minute: the window fills the
            # screen, and does it again the other way round.
            q.mouse_goto(24, 272)              # the "Ustaw" icon again
            q.mouse_click()
            time.sleep(4)
            q.mouse_goto(600, 200)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("okno otwarte na swoim miejscu", is_win(at(400, 200)))

            q.mouse_goto(WX + 100, WY + 4)
            time.sleep(0.4)
            q.mouse_click()
            time.sleep(0.15)
            q.mouse_click()                    # the second one, quickly
            time.sleep(2.5)
            q.mouse_goto(320, 300)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("dwa klikniecia w pasek tytulu rozwijaja okno na caly "
                  "ekran", is_win(at(20, 300)) and is_win(at(w - 4, 300)))
            check("ale nie na pasek zadan",
                  is_bar(at(320, h - TB_H + 6)))

            q.mouse_goto(200, 4)
            time.sleep(0.4)
            q.mouse_click()
            time.sleep(0.15)
            q.mouse_click()
            time.sleep(2.5)
            q.mouse_goto(600, 200)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("i drugie dwa klikniecia oddaja mu wlasny rozmiar",
                  is_win(at(400, 200)) and not is_win(at(20, 300)))

            q.sendkey("alt-f4")                # away with it again
            time.sleep(3)
            w, h, at = q.screen_raw()
            check("po zamknieciu pasek zadan jest jak byl",
                  len(bar_buttons(at, w, row)) == after)

            # --- the menu under the right button ------------------------
            MX, MY = 500, 300
            q.mouse_goto(MX, MY)
            time.sleep(0.5)
            q.mouse_right_click()
            time.sleep(1.5)
            w, h, at = q.screen_raw()

            check("prawy przycisk na pulpicie otworzyl menu POD KURSOREM",
                  is_win(at(MX + 100, MY + 2 * MENU_ROW + 8)))
            check("i ma swoja ramke, a nie samo tlo",
                  near(at(MX, MY + 2 * MENU_ROW + 8), 150))
            # The pointer sits on the first entry, so that entry wears
            # the accent colour - a menu that does not answer the
            # pointer is a list of words that happens to be clickable
            check("pozycja pod kursorem jest podswietlona",
                  is_accent(at(MX + 100, MY + 8)))
            check("a dalsza juz nie", not is_accent(
                  at(MX + 100, MY + 2 * MENU_ROW + 8)))
            # It came to the hand: nothing opened over the Menu button,
            # where the menu stands when that button is what opened it
            check("i nic nie wyskoczylo nad przyciskiem Menu",
                  not is_win(at(60, 430)))

            # --- a click on an entry starts the program -----------------
            # The second entry is the clock, and what it draws is a
            # TIME - so a window that merely appeared and a program
            # that actually runs are not the same assertion. We read
            # the digits with the font the kernel drew them with;
            # counting dark pixels would count the kernel's own window
            # underneath, whose button carries dark text too.
            q.mouse_goto(MX + 60, MY + MENU_ROW + 8)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(4)
            q.mouse_goto(600, 400)
            time.sleep(1.5)
            w, h, at = q.screen_raw()

            check("menu zamknelo sie po wyborze",
                  not is_win(at(MX + 100, MY + 2 * MENU_ROW + 8)))
            shown = q.read_text(WX + 1 + 8, WY + 11 + 6, 8, 1)
            check("program z menu naprawde chodzi - zegar rysuje "
                  "godzine ('%s')" % shown, shown.count(":") == 2)
            check("i ma swoj przycisk na pasku",
                  len(bar_buttons(at, w, row)) == after + 1)

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
            check("prawy przycisk na oknie nie otwiera menu",
                  not is_accent(at(WX + 100, WY + 28)))
            check("i zamyka to, ktore bylo otwarte",
                  not is_win(at(600, 350 + 2 * MENU_ROW + 8)))

            # --- the menu never hangs off the screen ----------------------
            # Clicked in the corner it slides back: to the left of the
            # right-hand edge, and above the taskbar
            q.mouse_goto(620, 450)
            time.sleep(0.5)
            q.mouse_right_click()
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            top = h - TB_H - MENU_N * MENU_ROW
            check("menu przy krawedzi zjechalo w lewo i w gore",
                  is_win(at(w - 20, top + 2 * MENU_ROW + 8)))
            check("nie wychodzi poza prawa krawedz",
                  not is_win(at(w - MENU_W - 12, top + 2 * MENU_ROW + 8)))
            check("i nie zachodzi na pasek zadan",
                  is_bar(at(w - 20, h - TB_H + 6)))

            # --- and Alt+F4 on a window that came from the menu ----------
            q.sendkey("alt-f4")
            time.sleep(3)
            w, h, at = q.screen_raw()
            log = q.serial()
            check("Alt+F4 zamyka takze okno uruchomione z menu",
                  "proces 'gclock' zakonczyl sie (kod 0)" in log)
            gone = q.read_text(WX + 1 + 8, WY + 11 + 6, 8, 1)
            check("zegar zniknal z ekranu ('%s')" % gone,
                  gone.count(":") != 2)
            check("i z paska zadan", len(bar_buttons(at, w, row)) == after)
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    print("--- to samo na jadrze 64-bitowym ---")
    sixty_four()
    return summary()


sys.exit(main())
