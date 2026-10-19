#!/usr/bin/env python3
"""ThreadOS smoke test: notifications, and the search.

Two things the desktop offers that are not windows.

A notification is a card at the top right: a title and a line, for
five seconds, newest on top. The shell sends one ('notify'), a program
sends one (sys_notify), the desktop sends one when it saves a picture of
the screen. The test reads the cards in the lettering they are written
in, sees the newest on top, sees a click send one away, and sees them go
by themselves.

The search is Ctrl+Space: a field under the bar, and what is typed is
looked for among the programs, the windows, the settings and the files
at the top of the disk - the names that begin with it first. Enter takes
what is picked, the arrows pick something else, Esc closes it. The test
types, reads what was found, and takes it: a program starts, a window
comes up out of the dock, a file opens in the editor.

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


def note_x(w=SCREEN_W):
    return w - NOTE_RIGHT - NOTE_W


def note_y(i):
    return NOTE_TOP + i * (NOTE_H + NOTE_GAP)


def card(q, i):
    """(title, line) of the i-th card from the top"""
    scr = q.screen_raw()
    x, y = note_x(), note_y(i)
    title = q.read_ui(x + NOTE_TEXT_X, y + NOTE_TITLE_Y, NOTE_W - 60,
                      px=NOTE_PX, face="bold", fg=DARK["menu_text"],
                      screen=scr)
    line = q.read_ui(x + NOTE_TEXT_X, y + NOTE_BODY_Y, NOTE_W - 60,
                     px=NOTE_PX, fg=DARK["menu_text"], screen=scr)
    return title, line


def search_x(w=SCREEN_W):
    return (w - SEARCH_W) // 2


def row_y(i):
    return SEARCH_Y + SEARCH_FIELD_H + 8 + i * SEARCH_ROW


def found(q):
    """What the search lists: (name, kind, picked) per row"""
    w, h, at = scr = q.screen_raw()
    x = search_x()
    out = []
    for i in range(SEARCH_ROWS):
        y = row_y(i)
        picked = near(at(x + 12, y + SEARCH_ROW // 2), DARK["accent"], 6)
        base = ui_font_baseline_in(MENU_PX, y, SEARCH_ROW)
        ink = DARK["accent_text"] if picked else DARK["menu_text"]
        name = q.read_ui(x + 16 + SEARCH_ICON + 12, base, 260, px=MENU_PX,
                         fg=ink, screen=scr)
        if not name:
            break
        kind = q.find_ui(x + SEARCH_W - 90, x + SEARCH_W - 10, base,
                         px=UI_PX, fg=ink if picked else DARK["title_dim"],
                         screen=scr)
        out.append((name, kind, picked))
    return out


def typed(q):
    return q.read_ui(search_x() + SEARCH_TEXT_X,
                     ui_font_baseline_in(SEARCH_PX, SEARCH_Y, SEARCH_FIELD_H),
                     300, px=SEARCH_PX, fg=DARK["menu_text"])


def search(q, text):
    q.sendkey("ctrl-spc")
    time.sleep(0.6)
    for c in text:
        q.sendkey({" ": "spc", ".": "dot"}.get(c, c))
        time.sleep(0.15)
    time.sleep(1)


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
            q.mouse_goto(300, 300)
            q.type_line("write notes.txt a line to find")
            time.sleep(1)

            # --- notifications --------------------------------------------
            # A program's first: it comes from ring 3
            q.type_line("run udtest")
            time.sleep(3)
            got = card(q, 0)
            check("a program's notification is a card at the top right, from "
                  "ring 3 (%s)" % (got,), got == ("udtest", "drawn and measured"))

            # Two from the shell, the newest on top. (Short: typing is a
            # tenth of a second a key, and the cards live five seconds.)
            q.click_desktop()             # the keys back to the shell
            time.sleep(0.5)
            q.type_line("notify First")
            q.type_line("notify Second")
            time.sleep(1)
            got, older = card(q, 0), card(q, 1)
            check("the shell's, the newest on top (%s)" % (got,),
                  got == ("Shell", "Second"))
            check("with the older one under it (%s)" % (older,),
                  older == ("Shell", "First"))

            q.click(note_x() + NOTE_W // 2, note_y(0) + NOTE_H // 2)
            q.mouse_goto(300, 300)
            time.sleep(0.5)
            check("a click sends a card away, and the one under it moves up "
                  "(%s)" % (card(q, 0),), card(q, 0) == ("Shell", "First"))
            time.sleep(6)
            w, h, at = q.screen_raw()
            bare = sum(1 for x in range(note_x(), note_x() + NOTE_W, 4)
                       for y in range(note_y(0), note_y(0) + NOTE_H, 4)
                       if at(x, y) == aurora_px(x, y))
            check("after five seconds they go by themselves (%d of %d points "
                  "the bare background)" % (bare, (NOTE_W // 4) * (NOTE_H // 4)),
                  bare == (NOTE_W // 4) * (NOTE_H // 4))

            q.sendkey("print")              # Print Screen
            got = ("", "")
            for _ in range(16):             # writing to the disk takes a while
                time.sleep(0.5)
                got = card(q, 0)
                if got[0]:
                    break
            check("a picture of the screen says it was saved (%s)" % (got,),
                  got[0] == "Screenshot" and "saved" in got[1])

            # --- the search -------------------------------------------------
            q.click_desktop()
            time.sleep(0.5)
            search(q, "cl")
            check("Ctrl+Space opens the search, and it shows what is typed "
                  "('%s')" % typed(q), typed(q) == "cl")
            rows = found(q)
            check("what begins with it comes first, and is picked (%s)"
                  % rows, rows[:1] == [("Clock", "Program", True)])
            q.sendkey("ret")
            time.sleep(3)
            check("Enter starts the program", q.dock_windows() == 2)
            w, h, at = q.screen_raw()
            check("and the search is gone", typed(q) == "")

            # A window: the clock is minimised, and found by its name
            q.click(*light_xy(*cascade(3, GCLOCK_W + 2, GCLOCK_H + TITLE_H + 2),
                              1))
            time.sleep(1.5)
            search(q, "clo")
            rows = found(q)
            check("an open window is found too (%s)" % rows,
                  ("Clock", "Window", False) in rows)
            q.sendkey("down")
            time.sleep(0.6)
            rows = found(q)
            check("the arrow picks the next one (%s)" % rows,
                  rows[1:2] == [("Clock", "Window", True)])
            q.sendkey("ret")
            time.sleep(2)
            cx, cy = cascade(3, GCLOCK_W + 2, GCLOCK_H + TITLE_H + 2)
            got = q.gclock_time(cx + 1, cy + CONTENT_DY)
            check("and taking it brings the window back out of the dock "
                  "('%s')" % got, got.count(":") == 2)

            # Esc closes it, and the keys go where they went before
            search(q, "zzz")
            check("nothing matches nonsense", found(q) == [])
            q.sendkey("esc")
            time.sleep(0.8)
            check("Esc closes the search", typed(q) == "")

            # A setting, and a file
            q.click_desktop()
            time.sleep(0.5)
            search(q, "moti")
            rows = found(q)
            check("a setting is found by its name (%s)" % rows,
                  ("motion", "Setting", True) in rows)
            q.sendkey("esc")
            time.sleep(0.5)
            before = q.dock_windows()
            search(q, "notes")
            rows = found(q)
            check("a file at the top of the disk is found (%s)" % rows,
                  ("notes.txt", "File", True) in rows)
            q.sendkey("ret")
            time.sleep(4)
            check("and taking it opens it in a window of its own",
                  q.dock_windows() == before + 1)
            check("in the editor", "exec 'gedit'" in q.serial())
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


sys.exit(main())
