#!/usr/bin/env python3
"""ProtonOS smoke test: the icons on the desktop come from a file.

Seven programs, decided when the kernel was built. Adding one meant
editing kernel/desktop.c and building a kernel - which is the same as
saying that a person cannot add one.

The list is now a file on the disk, one line per icon:

    program  napis  numer-obrazka

and nothing else. Plain text like everything else the system keeps: it
can be read with 'cat' and BUILT ON THE MACHINE ITSELF, with the shell
in the terminal and the redirections it already had -

    echo gterm Terminal 4 > pulpit.ikony
    echo gweb Internet 5 >> pulpit.ikony

which is what this test does, because a format nobody on the machine
can produce is a format for somebody else's machine.

What is checked: the file is really read (the kernel says so and the
desktop has as many icons as the file has lines, with the names and
pictures the file gave them), the icons still WORK (clicking one starts
its program), and a machine whose file says nothing usable keeps the
seven it was born with - a desktop with no icons is a machine with
nothing to click, and no broken line should be able to do that.

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

ICON_W, ICON_H = 32, 24
ORANGE = (255, 162, 0)


def icon_y(k):
    return 20 + k * 40


def is_tile(at, k):
    """Is there an orange tile at that place? Read at its corner, away
    from the picture in the middle."""
    return at(12, icon_y(k) + 4) == ORANGE


def label_of(q, k, cols=12):
    """The name under a tile, read through the kernel's own font. Only
    the white pixels count: the caption is drawn twice, once in black
    one pixel down, so that it reads on any wallpaper - and the shadow
    would make nonsense of a glyph looked up whole."""
    w, h, at = q.screen_raw()
    font = font8x8()
    y0 = icon_y(k) + ICON_H + 2
    out = ""
    for c in range(cols):
        pattern = []
        for y in range(8):
            bits = 0
            for x in range(8):
                if at(8 + c * 8 + x, y0 + y) == (255, 255, 255):
                    bits |= 1 << x
            pattern.append(bits)
        out += font.get(tuple(pattern), " " if not any(pattern) else "?")
    return out.rstrip()


def picture_of(at, k):
    x0 = 8 + (ICON_W - 12) // 2
    y0 = icon_y(k) + (ICON_H - 12) // 2
    return tuple(at(x0 + dx, y0 + dy) == (255, 255, 255)
                 for dy in range(12) for dx in range(12))


def write_list(q, lines):
    """Builds the file with the ring 3 shell - '>' for the first line
    and '>>' for the rest, exactly as a person would."""
    q.type_line("run sh")
    time.sleep(2)
    for i, line in enumerate(lines):
        q.type_line("echo %s %s %d" % (line[0], line[1], line[2]) +
                    (" > " if i == 0 else " >> ") + "pulpit.ikony")
        time.sleep(1.5)
    q.type_line("exit")
    time.sleep(1.5)


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    wanted = [("gterm", "Terminal", 4),
              ("gweb", "Internet", 5),
              ("gsettings", "Ustawienia", 6)]

    try:
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
        try:
            q.wait_for("ProtonOS shell", timeout=40)
            write_list(q, wanted)
            # The kernel's own two windows sit over the left-hand
            # column, and the icon captions run under them - so they go
            # out of the way before anything is read off the screen

            shown = q.command("cat pulpit.ikony", "gsettings", timeout=30)
            for app, label, art in wanted:
                check("plik ma wiersz dla '%s'" % app,
                      "%s %s %d" % (app, label, art) in shown)

            q.type_line("desktop")
            time.sleep(6)
            q.mouse_home()
            for bx, by in ((146, 66), (286, 101)):    # the two [_] buttons
                q.mouse_goto(bx, by)
                q.mouse_click()
                time.sleep(1)
            q.mouse_goto(560, 300)
            time.sleep(2)

            log = q.serial()
            check("jadro wczytalo liste ikon i mowi, ile",
                  "ikony z pliku 'pulpit.ikony': 3" in log)

            w, h, at = q.screen_raw()
            check("na pulpicie sa trzy kafelki",
                  is_tile(at, 0) and is_tile(at, 1) and is_tile(at, 2))
            check("i ani jednego wiecej", not is_tile(at, 3))

            for k, (app, label, art) in enumerate(wanted):
                got = label_of(q, k)
                check("kafelek %d nosi napis z pliku ('%s')" % (k, got),
                      got == label)

            pics = [picture_of(at, k) for k in range(3)]
            check("i kazdy ma inny obrazek",
                  pics[0] != pics[1] and pics[1] != pics[2] and
                  pics[0] != pics[2])

            # The icons still do what icons do
            q.mouse_goto(24, icon_y(0) + 12)
            q.mouse_click()
            time.sleep(4)
            log = q.serial()
            check("klik w ikone z pliku uruchamia jej program",
                  "exec 'gterm'" in log)
        finally:
            q.close()

        # --- a list that says nothing usable ---------------------------
        print("--- plik bez sensownych wierszy ---")
        slog2 = tempfile.mktemp(suffix=".log")
        q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog2)
        try:
            q2.wait_for("ProtonOS shell", timeout=40)
            # A file with nothing in it but a comment: the lines that
            # do not parse are passed over, and what is left is nothing
            q2.command("write pulpit.ikony #tylko-komentarz", "zapisano",
                       timeout=30)
            q2.type_line("desktop")
            time.sleep(6)
            q2.mouse_home()
            q2.mouse_goto(560, 300)
            time.sleep(2)

            w, h, at = q2.screen_raw()
            check("pulpit wraca do swoich siedmiu ikon",
                  is_tile(at, 0) and is_tile(at, 6))
            check("i pierwsza z nich jest ta, co zawsze ('App')",
                  label_of(q2, 0) == "App")
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
