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
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary
from ptest import _kernel_define as define

DARK = theme("dark")
# The editor's size (user/gedit.c), which says where in the cascade its
# window opens
GEDIT_W = define("user/gedit.c", "W0")
GEDIT_H = define("user/gedit.c", "H0")
# The desktop's frames a second at the most, and how many of them a
# card lives (NOTE_LIFE, '(5 * FRAME_HZ)' in kernel/desktop.c)
FRAME_HZ = define("kernel/desktop.c", "FRAME_HZ")


def _note_frames():
    full = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                        "..", "kernel", "desktop.c")
    with open(full) as fh:
        expr = re.search(r"#define\s+NOTE_LIFE\s+([^/\n]+)", fh.read())
    return eval(expr.group(1).replace("/", "//"), {}, {"FRAME_HZ": FRAME_HZ})


NOTE_FRAMES = _note_frames()


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
            # At the top of the disk, where the search looks for files;
            # the session's shell stands in its home folder
            q.command("write /notes.txt a line to find",
                      "'/notes.txt' written", timeout=10)
            # How fast the desktop draws, by its own count: a card lives
            # a number of its frames, not of seconds
            fps0 = q.frame_rate()

            # --- notifications --------------------------------------------
            # A program's first: it comes from ring 3. 'runw' and not
            # 'run', which would wait for the program to end and keep
            # the shell from the commands below
            q.type_line("runw udtest")
            time.sleep(3)
            got = card(q, 0)
            check("a program's notification is a card at the top right, from "
                  "ring 3 (%s)" % (got,), got == ("udtest", "drawn and measured"))

            # Two from the shell, the newest on top. (Short: typing is a
            # tenth of a second a key, and the cards live five seconds.)
            q.click_desktop()             # the keys back to the shell
            time.sleep(0.5)
            # (where the top card stands, and the bare background there
            # - worked out now, so that looking for the card to be gone
            # is a picture of the screen and nothing else)
            spots = [(x, y) for x in range(note_x(), note_x() + NOTE_W, 4)
                     for y in range(note_y(0), note_y(0) + NOTE_H, 4)]
            ground = [aurora_px(x, y) for x, y in spots]
            q.type_line("notify First")
            posted = time.time()
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
            # (and the rate again while the card is there: its glass is
            # blurred afresh every frame, which slows them down)
            fps1 = q.frame_rate()
            # A card lives five seconds' worth of frames (NOTE_LIFE in
            # kernel/desktop.c); on a host busy with other machines the
            # frames come slower than fifty a second, so the cards are
            # looked for until they are gone, and how long the older one
            # stayed is held to its frames at the slowest of the rates the
            # desktop says it draws, before, during and after - twice as
            # long, for a rate that dips between the readings, and three
            # seconds for the typing and the reading. (It cannot go
            # sooner than five seconds: the desktop draws fifty a second
            # at the most.)
            total = len(spots)
            bare, gone = 0, None
            while time.time() < posted + 90:
                w, h, at = q.screen_raw()
                bare = sum(1 for (x, y), g in zip(spots, ground)
                           if at(x, y) == g)
                if bare == total:
                    gone = time.time() - posted
                    break
                time.sleep(0.3)
            q.click_desktop()             # (the keys stay the shell's)
            rates = [r for r in (fps0, fps1, q.frame_rate()) if r]
            fps = min(rates) if rates else None
            bound = 2 * NOTE_FRAMES / fps + 3 if fps else 0
            check("after five seconds they go by themselves (%s, by %.1f s: "
                  "%d frames at %s a second of %s; %d of %d points the bare "
                  "background)" % ("%.1f s" % gone if gone else "still there",
                                   bound, NOTE_FRAMES, fps, rates, bare,
                                   total),
                  gone is not None and NOTE_FRAMES / FRAME_HZ - 0.5 <= gone
                  <= bound)

            q.sendkey("print")              # Print Screen
            got = ("", "")
            # writing to the disk takes a while
            deadline = time.time() + 60
            while time.time() < deadline:
                time.sleep(0.5)
                got = card(q, 0)
                if got[0] == "Screenshot":
                    break
            check("a picture of the screen says it was saved (%s)" % (got,),
                  got[0] == "Screenshot" and "saved" in got[1])

            # --- the search -------------------------------------------------
            # The program's window goes down to the dock first: the
            # search is glass, and a row that is not picked is read on
            # whatever lies under it - the window's white shapes,
            # blurred, are no ground to read letters on. It keeps its
            # place in the cascade, and its tile.
            q.click(*light_xy(94, 54, 1))
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
            # The third window: the program's, the clock's, and this one
            # at the next step of the cascade - and its title bar says
            # which program it is and which file it holds
            ex, ey = cascade(4, GEDIT_W + 2, GEDIT_H + TITLE_H + 2)
            title = ""
            deadline = time.time() + 10
            while time.time() < deadline:
                title = q.window_title(ex, ey, GEDIT_W + 2)
                if "notes.txt" in title:
                    break
                time.sleep(0.3)
            check("in the editor ('%s')" % title, title.startswith("Editor"))
            check("which has the file open - its name is in the title "
                  "('%s')" % title,
                  title.startswith("Editor: ") and title.endswith("notes.txt"))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


sys.exit(main())
