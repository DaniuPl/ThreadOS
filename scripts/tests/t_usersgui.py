#!/usr/bin/env python3
"""ProtonOS smoke test: asking for the password in the graphical part.

The kernel refuses a plain user who tries to change the system. On a
console that is the end of the story - the person types 'sudo' and the
shell asks. In a window there is no shell to ask, and an application in
ring 3 cannot raise its own rights: it can only be told 'not allowed'
and do something about it.

What it does about it is ask, and that is what this group is for. Both
of the machine's windows that need it ask in their own window, with
the keyboard they already have:

  the terminal, on 'sudo <command>': the line turns into a password
  line, what is typed shows as stars, and the command runs once - after
  which the terminal is a plain user again

  the settings window, when the write comes back refused: it says so,
  asks for the password under the setting being changed, and tries the
  same write again

Both go through the same syscall, and a wrong password gets the same
answer in both: nothing happens and it says so.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, font8x8

# The terminal opens in slot 2: content at (95,65), padded by 4, rows
# 10 apart, 400x240 of content.
WX, WY = 94, 54
CX, CY = WX + 1, WY + 11
PAD, ROW_H = 4, 10
COLS, ROWS = (400 - 8) // 8, (240 - 8) // 10


def read_rows(q, ox, oy, cols, rows):
    w, h, at = q.screen_raw()
    font = font8x8()
    counts = {}
    for r in range(rows):
        for y in range(8):
            for x in range(ox, ox + cols * 8, 2):
                p = at(x, oy + r * ROW_H + y)
                counts[p] = counts.get(p, 0) + 1
    bg = max(counts, key=counts.get)

    out = []
    for r in range(rows):
        line = ""
        for c in range(cols):
            pattern = []
            for y in range(8):
                bits = 0
                for x in range(8):
                    if at(ox + c * 8 + x, oy + r * ROW_H + y) != bg:
                        bits |= 1 << x
                pattern.append(bits)
            line += font.get(tuple(pattern), " " if not any(pattern) else "?")
        out.append(line.rstrip())
    return out


def wait_row(q, want, geom, tries=15):
    seen = []
    for _ in range(tries):
        seen = read_rows(q, **geom)
        if any(want in r for r in seen):
            return seen
        time.sleep(0.5)
    return seen


def read_band(q, ox, oy, cols):
    """One line of text off the screen, eight pixels tall.

    The settings window puts its rows fourteen pixels apart and its
    notes eleven, so there is no single grid to read it by - but every
    line is still eight pixels of the same font, and one line is all
    these assertions need."""
    w, h, at = q.screen_raw()
    font = font8x8()
    counts = {}
    for y in range(8):
        for x in range(ox, ox + cols * 8):
            p = at(x, oy + y)
            counts[p] = counts.get(p, 0) + 1
    bg = max(counts, key=counts.get)

    line = ""
    for c in range(cols):
        pattern = []
        for y in range(8):
            bits = 0
            for x in range(8):
                if at(ox + c * 8 + x, oy + y) != bg:
                    bits |= 1 << x
            pattern.append(bits)
        line += font.get(tuple(pattern), " " if not any(pattern) else "?")
    return line.rstrip()


def window_says(q, ox, y_from, y_to, want, tries=8):
    """Is that text anywhere in the band of lines? The status line of
    the settings window moves with the length of the description above
    it, so we look through the few rows it can be on."""
    for _ in range(tries):
        # Every row, not every third: a glyph read two pixels off its
        # own line decodes into nothing at all, so there is no such
        # thing as being nearly on it.
        for oy in range(y_from, y_to):
            if want in read_band(q, ox, oy, 20):
                return True
        time.sleep(0.6)
    return False


def typed(q, text):
    for ch in text:
        q.sendkey(q.KEYNAMES.get(ch, ch))
        time.sleep(0.12)


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    geom = dict(ox=CX + PAD, oy=CY + PAD, cols=COLS, rows=ROWS)

    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("pulpit wstal", q.wait_desktop(timeout=40))
            q.mouse_home()

            # --- the terminal ------------------------------------------
            q.mouse_goto(24, 192)          # the 'Term' icon
            q.mouse_click()
            time.sleep(3)
            q.mouse_goto(CX + 200, CY + 150)
            q.mouse_click()
            time.sleep(1)
            q.mouse_goto(600, 380)
            time.sleep(1)

            q.type_line("whoami")
            rows = wait_row(q, "plain user", geom)
            check("terminal wstaje jako zwykly uzytkownik",
                  "plain user" in "\n".join(rows))

            # A program in ring 3 asking to delete a system file is
            # refused by the KERNEL - and rm says which refusal it was
            q.type_line("rm swap.sys")
            rows = wait_row(q, "uprawnien", geom)
            check("program dostaje odmowe, a nie 'nie ma pliku'",
                  "brak uprawnien" in "\n".join(rows))

            # --- sudo in the window ------------------------------------
            q.type_line("sudo cp termout system.cfg")
            time.sleep(1.5)
            rows = read_rows(q, **geom)
            check("terminal pyta o haslo",
                  any(r.startswith("password:") for r in rows))

            # A wrong one changes nothing and says so
            typed(q, "zlehaslo")
            q.sendkey("ret")
            rows = wait_row(q, "wrong password", geom)
            check("zle haslo odrzucone", "wrong password" in "\n".join(rows))

            q.type_line("sudo cp termout system.cfg")
            time.sleep(1.5)
            # What is typed shows as stars: the line has as many of them
            # as there were keys, and none of the letters
            typed(q, "proton")
            time.sleep(0.5)
            rows = read_rows(q, **geom)
            last = [r for r in rows if r][-1]
            check("wpisywane haslo widac jako gwiazdki ('%s')" % last,
                  "******" in last and "proton" not in last)

            q.sendkey("ret")
            time.sleep(3)
            rows = wait_row(q, "sudo cp", geom)
            body = "\n".join(rows)
            check("komenda wykonala sie z prawami admina",
                  "brak uprawnien" not in body.split("sudo cp")[-1])

            q.type_line("whoami")
            time.sleep(2)
            rows = read_rows(q, **geom)
            # Twice: once at the start of the session and once now,
            # after the one command that was run as an admin
            check("a po niej terminal znowu jest zwyklym uzytkownikiem",
                  "\n".join(rows).count("plain user") >= 2)

            # --- the settings window ------------------------------------
            q.mouse_goto(24, 272)          # the 'Ustaw' icon
            q.mouse_click()
            time.sleep(3)
            q.mouse_goto(600, 380)
            time.sleep(1.5)

            # Slot 3: (106,66), content from (107,77). The 'tlo' row is
            # the fourth: ROW_Y=22 + 3*14 = 64
            SX, SY = 107, 77
            q.mouse_goto(SX + 40, SY + 64)
            q.mouse_click()
            time.sleep(1)
            q.mouse_click()                # cycle the value
            time.sleep(1)
            q.sendkey("ret")               # ask to write it
            time.sleep(2)
            q.mouse_goto(600, 380)
            time.sleep(1.5)

            # The status line sits under the description of the
            # setting, so its exact row depends on how long that is -
            # we look through the band it can be in.
            check("okno ustawien mowi, ze potrzeba admina",
                  window_says(q, SX + 6, SY + 100, SY + 200, "haslo admina"))

            typed(q, "zlehaslo")
            q.sendkey("ret")
            time.sleep(2)
            q.mouse_goto(600, 380)
            time.sleep(1.5)
            check("zle haslo w oknie tez niczego nie zapisuje",
                  window_says(q, SX + 6, SY + 100, SY + 200, "NIE zapisane"))

            q.sendkey("ret")               # ask again
            time.sleep(1.5)
            typed(q, "proton")
            q.sendkey("ret")
            time.sleep(2.5)
            q.mouse_goto(600, 380)
            time.sleep(1.5)
            check("dobre haslo zapisuje ustawienie",
                  window_says(q, SX + 6, SY + 100, SY + 200,
                              "zapisane na dysku"))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
