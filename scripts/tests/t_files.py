#!/usr/bin/env python3
"""ProtonOS smoke test: the file manager.

What it replaced listed eight names in a window the size of a postage
stamp and showed the first hundred bytes of one of them. That proved
the path "disk -> click -> read -> drawing"; it was not a file
manager. A file manager is where a person GOES to find a file: it says
how big things are, it walks into directories and back out of them, it
hands a file to the program that opens it, and it throws one away when
asked twice.

The assertions follow exactly that. The sizes are checked against a
file whose length the test knows, because a column of numbers that
comes from nowhere proves nothing. Deleting is checked in both halves:
the first click only ASKS - and the file is still there afterwards -
and the second one does it, which the kernel's own shell then confirms
by failing to read the file. A directory is not deleted at all.

The window's name is checked too: it says which directory it is
showing, and changes when we walk into one. Two file managers open on
two directories would be two windows called "Pliki" otherwise.

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

# The window: slot 2 of the cascade, content a pixel right and a title
# bar down; gfiles asks for 320x220.
WX, WY = 94, 54
CX, CY = WX + 1, WY + 11
CW, CH = 320, 220
ROW_Y0 = 18
ROW_H = 10
DEL_X = CW - 48
EDIT_X = CW - 48 - 56 - 4

TEXT = "Wyniki pomiarow"          # what the test writes, and its length


def is_grey(px):
    return all(abs(c - 227) <= 8 for c in px)


def is_accent(px):
    r, g, b = px
    return b > 120 and r < 90 and g < 90


def read_at(at, x, y, cols):
    """Text inside the window: anything that is not the window's own
    grey is ink. A chosen row has the accent colour behind it, so the
    background is 'grey or accent', not 'light'."""
    font = font8x8()
    out = ""
    for c in range(cols):
        pattern = []
        for yy in range(8):
            bits = 0
            for xx in range(8):
                p = at(CX + x + c * 8 + xx, CY + y + yy)
                if not is_grey(p) and not is_accent(p):
                    bits |= 1 << xx
            pattern.append(bits)
        out += font.get(tuple(pattern), " " if not any(pattern) else "?")
    return out.strip()


def title(at, cols=24):
    """The window's own name, white on the title bar."""
    font = font8x8()
    out = ""
    for c in range(cols):
        pattern = []
        for yy in range(8):
            bits = 0
            for xx in range(8):
                p = at(WX + 4 + c * 8 + xx, WY + 2 + yy)
                if all(v > 200 for v in p):
                    bits |= 1 << xx
            pattern.append(bits)
        out += font.get(tuple(pattern), " " if not any(pattern) else "?")
    return out.strip()


def rows(at, n=8):
    out = []
    for i in range(n):
        name = read_at(at, 6, ROW_Y0 + i * ROW_H, 14)
        if not name:
            break
        # The size is drawn right-aligned, its left edge depending on
        # how many digits it has - so the grid is anchored to the RIGHT
        # edge, which does not move: the last glyph ends six pixels
        # short of the window's edge.
        out.append((name, read_at(at, CW - 6 - 12 * 8,
                                  ROW_Y0 + i * ROW_H, 12)))
    return out


def click(q, x, y, wait=1.5):
    q.mouse_goto(x, y)
    time.sleep(0.5)
    q.mouse_click()
    time.sleep(wait)


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

            # Something to look at: a file whose length we know, and a
            # directory to walk into
            q.type_line("write raport.txt " + TEXT)
            time.sleep(1)
            q.type_line("run mkdir dokumenty")
            time.sleep(2)

            click(q, 24, 112, wait=4)          # the "Pliki" icon
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()

            listed = rows(at)
            names = [r[0] for r in listed]
            check("menedzer listuje zawartosc dysku (%s)"
                  % ", ".join(names), "raport.txt" in names)
            check("i katalog tez (%s)" % ", ".join(names),
                  "dokumenty" in names)

            # The size column against a file whose length we know: the
            # shell writes the text and a newline
            size = dict(listed).get("raport.txt", "")
            check("przy pliku stoi jego rozmiar ('%s' przy %d znakach "
                  "tekstu)" % (size, len(TEXT)),
                  size.startswith(str(len(TEXT))) or
                  size.startswith(str(len(TEXT) + 1)))
            check("a przy katalogu slowo zamiast liczby ('%s')"
                  % dict(listed).get("dokumenty", ""),
                  dict(listed).get("dokumenty", "") == "katalog")

            check("naglowek mowi, gdzie jestesmy ('%s')"
                  % read_at(at, 4, 2, 20), read_at(at, 4, 2, 20).startswith("/"))
            check("i okno nosi nazwe katalogu ('%s')" % title(at),
                  title(at) == "Pliki: /")

            # --- walking into a directory -------------------------------
            i = names.index("dokumenty")
            click(q, CX + 60, CY + ROW_Y0 + i * ROW_H + 3)
            click(q, CX + EDIT_X + 20, CY + 4, wait=2.5)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("'Otworz' na katalogu wchodzi do niego ('%s')"
                  % read_at(at, 4, 2, 20),
                  read_at(at, 4, 2, 20).startswith("/dokumenty"))
            check("i okno mowi to samo ('%s')" % title(at),
                  title(at) == "Pliki: /dokumenty")
            check("a w srodku pusto", rows(at) == [])

            # --- and back out -------------------------------------------
            click(q, CX + 160, CY + 120, wait=1)   # the focus into the window
            q.sendkey("backspace")
            time.sleep(2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("Backspace wraca wyzej ('%s')" % title(at),
                  title(at) == "Pliki: /")
            listed = rows(at)
            names = [r[0] for r in listed]
            check("i widac znowu to, co bylo", "raport.txt" in names)

            # --- deleting asks first ------------------------------------
            i = names.index("raport.txt")
            click(q, CX + 60, CY + ROW_Y0 + i * ROW_H + 3)
            click(q, CX + DEL_X + 16, CY + 4, wait=2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            asked = read_at(at, 4, CH - 10, 36)
            check("pierwszy klik w 'Usun' tylko pyta ('%s')" % asked,
                  "Usunac" in asked and "raport.txt" in asked)
            check("i plik dalej jest",
                  "raport.txt" in [r[0] for r in rows(at)])

            click(q, CX + DEL_X + 16, CY + 4, wait=2.5)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("drugi klik usuwa",
                  "raport.txt" not in [r[0] for r in rows(at)])

            # Confirmed by something that is not this program
            q.mouse_goto(500, 380)
            q.mouse_click()                    # the desktop takes the keys
            time.sleep(1)
            seen = len(q.serial())
            q.type_line("cat raport.txt")
            time.sleep(2)
            after = q.serial()[seen:]
            check("i pliku naprawde nie ma na dysku",
                  TEXT not in after)

            # --- a directory is not deleted -----------------------------
            click(q, CX + 160, CY + 120, wait=1)
            w, h, at = q.screen_raw()
            names = [r[0] for r in rows(at)]
            i = names.index("dokumenty")
            click(q, CX + 60, CY + ROW_Y0 + i * ROW_H + 3)
            click(q, CX + DEL_X + 16, CY + 4, wait=2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            said = read_at(at, 4, CH - 10, 36)
            check("katalogu nie usuwa, i mowi dlaczego ('%s')" % said,
                  "katalog" in said)
            check("a katalog dalej jest",
                  "dokumenty" in [r[0] for r in rows(at)])
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
