#!/usr/bin/env python3
"""ThreadOS smoke test: Polish letters from the keyboard.

The fonts have had the Polish letters for a long time; the keyboard had
no way to them. Now AltGr - the right Alt - with a, c, e, l, n, o, s,
x and z gives ą ć ę ł ń ó ś ź ż (with Shift the capitals): the "Polish
programmer's" layout every Polish keyboard is used with. Each letter is
two bytes of UTF-8, and what is asserted is that everything that takes
typing treats them as one letter:

  the editor shows the pangram "Zażółć gęślą jaźń" letter for letter -
  read off the screen against FreeType's own glyphs - its caret steps
  over a letter whole, Backspace takes a letter whole and Ctrl+Z gives
  it back whole; the file saved holds the UTF-8 bytes, which the shell
  prints back

  the terminal takes them on its command line and shows what a program
  wrote back with them

  and a USB keyboard - where AltGr is a bit in the report, not a key -
  types them the same

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary
from ptest import _kernel_define as define

PL = "ąćęłńóśźżĄĆĘŁŃÓŚŹŻ"
E = "user/gedit.c"
KEYS = {"ą": "a", "ć": "c", "ę": "e", "ł": "l", "ń": "n", "ó": "o", "ś": "s",
        "ź": "x", "ż": "z"}


def keys_for(text):
    """QEMU key names that type text on the Polish programmer's layout"""
    out = []
    for ch in text:
        low = ch.lower()
        if low in KEYS:
            out.append(("shift-" if ch != low else "") + "alt_r-" + KEYS[low])
        elif ch == " ":
            out.append("spc")
        elif ch.isupper():
            out.append("shift-" + ch.lower())
        else:
            out.append(Qemu.KEYNAMES.get(ch, ch))
    return out


def typed(q, text):
    for k in keys_for(text):
        q.sendkey(k)
        time.sleep(0.08)
    time.sleep(0.8)


def editor_frame():
    w, h = define(E, "W0"), define(E, "H0")
    x, y = cascade(2, w + 2, h + TITLE_H + 2)
    return x + 1, y + CONTENT_DY


def editor_lines(q, n=2):
    cx, cy = editor_frame()
    return q.read_cells(cx + define(E, "TEXT_X"), cy + define(E, "PAD"), 40, n,
                        cell=define(E, "CELL"), row_h=define(E, "ROW_H"),
                        base=define(E, "TEXT_BASE"), px=define(E, "TEXT_PX"),
                        extra=PL)


def editor(q):
    q.type_line("run gedit")
    time.sleep(4)
    q.mouse_goto(600, 420)
    pangram = "Zażółć gęślą jaźń"
    typed(q, pangram)
    got = editor_lines(q, 1)
    check("AltGr types the Polish letters: the pangram, letter for letter "
          "(%s)" % got, got == [pangram])

    q.sendkey("backspace")
    time.sleep(0.8)
    got = editor_lines(q, 1)
    check("Backspace takes the last letter whole (%s)" % got,
          got == ["Zażółć gęślą jaź"])
    q.sendkey("ctrl-z")
    time.sleep(0.8)
    got = editor_lines(q, 1)
    check("and Ctrl+Z gives it back whole (%s)" % got, got == [pangram])

    q.sendkey("left")
    q.sendkey("left")
    typed(q, "X")
    got = editor_lines(q, 1)
    check("the caret steps over letters, not bytes: two to the left and "
          "an X goes between 'a' and 'ź' (%s)" % got,
          got == ["Zażółć gęślą jaXźń"])
    q.sendkey("ctrl-z")
    q.sendkey("end")
    q.sendkey("ret")
    typed(q, "ĄĆĘŁŃÓŚŹŻ")
    got = editor_lines(q, 2)
    check("with Shift, the capitals (%s)" % got,
          got == [pangram, "ĄĆĘŁŃÓŚŹŻ"])

    # Saved, the file holds UTF-8 - which the shell prints back as is
    q.sendkey("ctrl-s")
    time.sleep(0.8)
    typed(q, "pl.txt\n")
    time.sleep(1.5)
    q.sendkey("alt-f4")
    time.sleep(2)
    q.click_desktop()
    seen = len(q.serial())
    q.type_line("cat pl.txt")
    time.sleep(3)
    # (the port's log is read as bytes; the letters are UTF-8 in it)
    out = q.serial()[seen:].encode("latin1").decode("utf-8", "replace")
    check("the file saved holds the letters as UTF-8 ('cat' prints them)",
          pangram in out and "ĄĆĘŁŃÓŚŹŻ" in out)


def terminal(q):
    q.click_desktop()
    q.type_line("run gterm")
    time.sleep(5)
    q.mouse_goto(600, 420)
    tx, ty = cascade(2, TERM_W + 2, TERM_H + TITLE_H + 2)
    cx, cy = tx + 1, ty + CONTENT_DY
    typed(q, "echo żółw ĄĘ\n")
    time.sleep(2)
    rows = q.read_cells(cx + TERM_PAD, cy + TERM_PAD, TERM_COLS, TERM_ROWS,
                        cell=TERM_CELL, row_h=TERM_ROW_H, base=TERM_BASE,
                        px=TERM_PX, extra=PL)
    rows = [r for r in rows if r]
    check("the terminal takes them on its line and shows the answer "
          "(%s)" % rows[-3:], "żółw ĄĘ" in rows and
          any(r.endswith("echo żółw ĄĘ") for r in rows))
    # "abc", the cursor before the b, an ś typed there, and two
    # Backspaces: the ś goes whole, then the a
    typed(q, "abc")
    q.sendkey("left")
    q.sendkey("left")
    typed(q, "ś")
    q.sendkey("backspace")
    q.sendkey("backspace")
    time.sleep(0.8)
    rows = q.read_cells(cx + TERM_PAD, cy + TERM_PAD, TERM_COLS, TERM_ROWS,
                        cell=TERM_CELL, row_h=TERM_ROW_H, base=TERM_BASE,
                        px=TERM_PX, extra=PL)
    rows = [r for r in rows if r]
    check("and edits the line letter by letter (%s)" % rows[-1:],
          rows[-1].endswith("> bc"))


def usb():
    q = Qemu(KERNEL, INITRD, usbkbd="xhci", serial=tempfile.mktemp(suffix=".log"))
    try:
        check("(USB) the desktop came up", q.wait_desktop(timeout=60))
        q.mouse_home()
        q.mouse_goto(600, 420)
        q.type_line("run gedit")
        time.sleep(4)
        typed(q, "Łódź")
        got = editor_lines(q, 1)
        check("(USB) AltGr on a USB keyboard types them too (%s)" % got,
              got == ["Łódź"])
    finally:
        q.close()


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()
            q.mouse_goto(600, 420)
            editor(q)
            terminal(q)
        finally:
            q.close()
        usb()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


sys.exit(main())
