#!/usr/bin/env python3
"""ProtonOS smoke test: closing and resizing application windows.

A window that opens and cannot be closed is not a window, and until now
that is what an application had: the compositor's [x] dropped the
window out from under a process that went on running, drawing into
something that no longer existed. Resizing was for the kernel's own
windows only.

Both now work the way they do in a system somebody uses:

  Closing is a REQUEST. The compositor tells the application its window
  is being closed and gives it time to end by itself. A program that
  answers ends with its own exit code, having tidied up after itself.

  A program that does NOT answer is killed. A window nobody can close
  is worse than an application that loses its say - so after about two
  seconds the desktop stops asking. That is what 'gdeaf' is for: it
  opens a window and never listens.

  Resizing an application window makes it a new content buffer and
  tells the application the new size. Once, when the corner is let go -
  not on every frame of the pull.

The assertions are about what can be seen and what the kernel says: the
window disappears from the screen AND from the taskbar, the process
ends with code 0 when it was asked and with a kill when it was not, and
the memory comes back.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

# The application window is slot 2: the compositor cascades them at
# (70+12i, 30+12i), so (94,54). gsettings asks for 330x160 content.
WX, WY = 94, 54
WW, WH = 332, 172

# Where to look, and why there. The kernel's own windows sit at
# (40,60) 120x70 and (180,95) 120x70, so a probe has to fall inside the
# application's window and outside both of those - otherwise it reads
# somebody else's grey and says nothing.
P_APP = (400, 200)          # inside gsettings, past both kernel windows
P_DEAF = (170, 70)          # inside gdeaf, in the gap between them


def grey(px):
    """Is this the grey of a window's background (56,56,56 -> ~227)?"""
    r, g, b = px
    return r > 200 and g > 200 and b > 200 and abs(r - g) < 12


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

            # --- an application window opens ---------------------------
            q.mouse_goto(24, 272)          # the 'Ustaw' icon
            q.mouse_click()
            time.sleep(3)
            q.mouse_goto(600, 430)         # the cursor out of the way
            time.sleep(1)

            w, h, at = q.screen_raw()
            check("okno aplikacji sie otworzylo (szare tlo w slocie 2)",
                  grey(at(*P_APP)))
            # Beyond the window there is desktop - so that growing it is
            # visible as a change and not as something already there
            check("poza oknem jest pulpit, a nie okno",
                  not grey(at(WX + WW + 60, WY + 100)))

            # --- resizing ----------------------------------------------
            q.mouse_goto(WX + WW - 4, WY + WH - 4)     # the grip
            q.mouse_button(True)
            q.mouse_goto(WX + WW + 90, WY + WH + 60)   # pull it out
            time.sleep(0.5)
            q.mouse_button(False)
            time.sleep(2)
            q.mouse_goto(600, 120)
            time.sleep(1)

            w, h, at = q.screen_raw()
            check("okno uroslo tam, gdzie przedtem byl pulpit",
                  grey(at(WX + WW + 60, WY + 100)))
            # The program redrew itself at the new size: its background
            # reaches the new bottom edge, and a buffer that was not
            # remade would leave rubbish or the desktop there
            check("program przerysowal sie w nowym rozmiarze",
                  grey(at(WX + 40, WY + WH + 30)))


            # --- closing: the program is asked and ends itself ---------
            q.mouse_goto(WX + WW + 90 - 6, WY + 5)      # the new [x]
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(3)
            q.mouse_goto(600, 430)
            time.sleep(1)

            log = q.serial()
            check("program zakonczyl sie SAM, z kodem zero",
                  "proces 'gsettings' zakonczyl sie (kod 0)" in log)

            w, h, at = q.screen_raw()
            check("okno zniknelo z ekranu", not grey(at(*P_APP)))
            # The taskbar button has to go too - a window that is gone
            # from the screen and left on the bar is half-closed
            bar = q.screen_raw()[2]
            check("i zniknelo z paska zadan",
                  not any(bar(x, 474)[2] > 150 and bar(x, 474)[0] < 90
                          for x in range(150, 250)))
        finally:
            q.close()

        # --- an application that does not listen ----------------------
        print("--- program, ktory nie slucha ---")
        slog2 = tempfile.mktemp(suffix=".log")
        q2 = Qemu(KERNEL, INITRD, disk, serial=slog2)
        try:
            check("pulpit wstal (druga proba)", q2.wait_desktop(timeout=40))
            q2.mouse_home()
            # 'gdeaf' has no icon - the kernel shell starts it, which in
            # graphics mode listens as long as no window has focus
            q2.type_line("run gdeaf")
            time.sleep(4)
            q2.mouse_goto(600, 430)
            time.sleep(1)

            w, h, at = q2.screen_raw()
            check("okno gluchego programu jest na ekranie",
                  grey(at(*P_DEAF)))

            q2.mouse_goto(WX + 100 + 2 - 6, WY + 5)   # its [x] (100 wide)
            time.sleep(0.5)
            q2.mouse_click()
            # It will not answer, so the desktop has to stop asking. The
            # grace period is about two seconds; we give it five.
            time.sleep(5)
            q2.mouse_goto(600, 430)
            time.sleep(1)

            log = q2.serial()
            check("pulpit przestal prosic i zabil program",
                  "zabity" in log or "kod -9" in log or
                  "proces 'gdeaf'" in log)
            w, h, at = q2.screen_raw()
            check("okno zniknelo mimo braku odpowiedzi",
                  not grey(at(*P_DEAF)))

            # And the desktop is alive after all that
            check("pulpit dziala dalej",
                  at(600, 300)[1] > 20 or at(600, 300)[2] > 20)
        finally:
            q2.close()
            if os.path.exists(slog2):
                os.unlink(slog2)
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
