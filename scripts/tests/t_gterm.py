#!/usr/bin/env python3
"""ThreadOS smoke test: the terminal in a window.

What was there before ran one command and drew what it printed. Every
command wiped out the one before it, the window was 240x150 whatever
the person did with it, there was no cursor, no history, no way back to
what had scrolled past and no way to change directory. It proved that a
program in ring 3 can start another one and read its output. It was not
a terminal.

A terminal is a session, and that is what is checked here - by READING
the window. Every assertion below decodes the pixels of the terminal's
window through the same 8x8 font the kernel draws with, so what is
tested is what a person would see, not what the program believes:

  the prompt and the command are echoed, and stay above the next one
  the output appears under its own command, in order
  the arrows bring back what was typed before
  the cursor can be moved and typing inserts where it stands
  'cd' changes the directory AND the prompt
  'a | b' is a pipe, and the right-hand side's output is what is shown
  a program that does not exist says so instead of doing nothing
  PgUp goes back to what scrolled away, and says that it did
  the window's size decides how much is visible - maximising it shows
  more lines, because the terminal asked the window and not a constant

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

# The window opens in slot 2 - the compositor cascades at (70+12i,
# 30+12i) - and asks for 400x240 of content, so the content starts at
# (95,65). The terminal pads by 4 and puts its rows 10 apart.
WX, WY = 94, 54
CX, CY = WX + 1, WY + CONTENT_DY
PAD, ROW_H = 4, 10
COLS, ROWS = (400 - 8) // 8, (240 - 8) // 10

BTN = 8                       # the title bar's [O] [_] [x] are 8x8


def read_rows(q, ox, oy, cols, rows):
    """The terminal's text, read off the screen.

    Every cell is eight by eight pixels; the pattern of eight bytes is
    looked up in the very font the kernel drew it with. The background
    is the commonest colour of the whole block, because in a character
    like '0' there are more glyph pixels than background ones."""
    w, h, at = q.screen_raw()
    font = font8x8()

    counts = {}
    for r in range(rows):
        for y in range(8):
            yy = oy + r * ROW_H + y
            for x in range(ox, ox + cols * 8, 2):
                p = at(x, yy)
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


def text(rows):
    return "\n".join(rows)


def wait_for_line(q, want, tries=20, **kw):
    """Waits for a WHOLE line to be exactly that text.

    The command is echoed the moment it is typed and its output arrives
    afterwards - the program runs while the terminal goes on drawing -
    so waiting for a substring can catch the echo and read the screen
    before the output is on it."""
    seen = []
    for _ in range(tries):
        seen = read_rows(q, **kw)
        if any(r == want for r in seen):
            return seen
        time.sleep(0.5)
    return seen


def wait_for_row(q, want, tries=15, **kw):
    """Waits until some row holds the text - the command runs in another
    process, so it is not there the moment Enter is pressed."""
    seen = []
    for _ in range(tries):
        seen = read_rows(q, **kw)
        if any(want in r for r in seen):
            return seen
        time.sleep(0.5)
    return seen


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    geom = dict(ox=CX + PAD, oy=CY + PAD, cols=COLS, rows=ROWS)

    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()

            # Three files for the Tab test at the end, made now: the
            # kernel's shell listens while no window has the focus, and
            # once the terminal is open and filling the screen there is
            # no desktop left to click on.
            q.type_line("write fileone content")
            time.sleep(1)
            q.type_line("write filetwo content")
            time.sleep(1)
            q.type_line("write other content")
            time.sleep(1.5)

            q.mouse_goto(24, 192)          # the 'Term' icon at (8,180)
            q.mouse_click()
            time.sleep(3)
            q.mouse_goto(CX + 200, CY + 150)
            q.mouse_click()                # focus, so keys go here
            time.sleep(1)
            q.mouse_goto(600, 380)         # the cursor out of the window
            time.sleep(1)

            rows = read_rows(q, **geom)
            check("the terminal opened and says hello",
                  "terminal" in text(rows))
            check("and it shows a prompt with the directory",
                  any(r.startswith("/>") for r in rows))

            # --- a command, its echo and its output --------------------
            q.type_line("seq 3")
            rows = wait_for_line(q, "3", **geom)
            body = text(rows)
            check("the command was echoed together with the prompt", "/> seq 3" in body)
            check("and under it is its output",
                  "1" in rows and "2" in rows and "3" in rows)

            # --- the session stays -------------------------------------
            q.type_line("date")
            rows = wait_for_row(q, "2030-", **geom)
            body = text(rows)
            check("the date is printed (the machine's clock)", "2030-06-13" in body)
            check("and the previous command is STILL above it - a session, "
                  "not one output", "/> seq 3" in body)

            # --- history ------------------------------------------------
            q.sendkey("up")
            q.sendkey("up")
            time.sleep(1)
            rows = read_rows(q, **geom)
            last = [r for r in rows if r.startswith("/>")][-1]
            check("the up arrow recalls the previous command ('%s')"
                  % last, last.strip().endswith("seq 3"))

            q.sendkey("ret")               # and it can be run again
            rows = wait_for_line(q, "3", **geom)
            time.sleep(1)
            rows = read_rows(q, **geom)
            check("a recalled command can be run",
                  text(rows).count("/> seq 3") >= 2)

            # --- moving inside the line and inserting -------------------
            # 'eq 4' with the cursor sent home and an 's' typed in front
            # of it is 'seq 4' - which only works if the cursor really
            # moves and typing really inserts
            for ch in "eq 4":
                q.sendkey({" ": "spc"}.get(ch, ch))
            q.sendkey("home")
            q.sendkey("s")
            time.sleep(0.5)
            rows = read_rows(q, **geom)
            last = [r for r in rows if r.startswith("/>")][-1]
            check("the cursor went back and the character landed IN THE MIDDLE of the line ('%s')"
                  % last.strip(), last.strip().endswith("seq 4"))
            q.sendkey("ret")
            rows = wait_for_line(q, "4", **geom)
            check("and a command put together that way works", "4" in rows)

            # --- cd changes the directory and the prompt ---------------
            q.type_line("mkdir dir")
            time.sleep(2)
            q.type_line("cd dir")
            rows = wait_for_row(q, "/dir>", **geom)
            check("cd changed the directory, and the prompt says so at once",
                  any(r.startswith("/dir>") for r in rows))
            q.type_line("cd /")
            time.sleep(2)

            # --- a pipe --------------------------------------------------
            q.type_line("seq 9 | wc")
            rows = wait_for_row(q, "lines=9", **geom)
            check("the pipe 'a | b' counts what the left side printed",
                  "lines=9" in text(rows))

            # --- a program that is not there ---------------------------
            q.type_line("nosuchprogram")
            rows = wait_for_row(q, "no such program", **geom)
            check("an unknown program ends with a message, not with silence",
                  "no such program" in text(rows))

            # --- scrolling back ------------------------------------------
            q.type_line("seq 40")
            rows = wait_for_line(q, "40", **geom)
            check("long output ends on the last line",
                  "40" in rows)
            check("and the start of the output scrolled out of the window", "1" not in rows)

            # One PgUp is half a screen, so it takes a few to get back
            # past forty lines of output to the command that made them
            for _ in range(4):
                q.sendkey("pgup")
                time.sleep(0.3)
            time.sleep(1)
            rows = read_rows(q, **geom)
            check("PgUp goes back to what scrolled away - as far as the first "
                  "the session's commands", "/> seq 3" in text(rows))
            check("and the terminal says we are not looking at the end",
                  "scrolled back" in text(rows))

            for _ in range(6):             # and back down to the end
                q.sendkey("pgdn")
                time.sleep(0.3)
            time.sleep(1)
            rows = read_rows(q, **geom)
            check("PgDn comes back to the end of the session",
                  "40" in rows and "scrolled back" not in text(rows))

            # --- the window's size decides how much is visible ---------
            # The [O] button maximises; the terminal asks the window for
            # its size, so it must show more lines than before
            before = len([r for r in read_rows(q, **geom) if r])
            q.mouse_goto(WX + 402 - 3 * BTN - 3 + BTN // 2, WY + 1 + TITLE_H // 2)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(3)
            q.mouse_goto(600, 380)
            time.sleep(2)

            w, h, at = q.screen_raw()
            big = dict(ox=1 + PAD, oy=CONTENT_DY + PAD,
                       cols=(w - 2 - 8) // 8,
                       rows=(h - 12 - (TITLE_H + 2) - 8) // ROW_H)
            rows = read_rows(q, **big)
            after = len([r for r in rows if r])
            check("once the window is stretched MORE session lines show "
                  "(%d against %d)" % (after, before), after > before)
            check("and it is still the same session - the commands from before "
                  "the resize are in place", "/> seq 40" in text(rows))
            check("the prompt is at the bottom edge of the bigger window",
                  any(r.startswith("/>") for r in rows[-2:]))

            # --- Tab finishes what is being typed -----------------------
            # A terminal where a file name has to be typed out in full,
            # correctly, every time is a terminal one types slowly at.
            q.type_line("cat oth", enter=False)
            time.sleep(0.5)
            q.sendkey("tab")
            time.sleep(1.5)
            q.sendkey("ret")
            rows = wait_for_row(q, "content", **big)
            check("Tab finishes a name when only one matches "
                  "(cat oth -> other)",
                  any("cat other" in r for r in rows))

            # Two names that start alike: as far as they agree, and the
            # candidates written out - going further would be guessing
            q.type_line("cat file", enter=False)
            time.sleep(0.5)
            q.sendkey("tab")
            time.sleep(2)
            rows = read_rows(q, **big)
            check("with several matches it lists the candidates (%s)"
                  % [r for r in rows if "fileone" in r][:1],
                  any("fileone" in r and "filetwo" in r for r in rows))

            # And what it added is the part they agree on: pressing
            # Enter now runs 'cat file', which has no such file
            q.sendkey("ret")
            time.sleep(2)
            rows = read_rows(q, **big)
            check("and it added only what they share (still 'cat file')",
                  any(r.startswith("/> cat file") and
                      not r.startswith("/> cat fileone") and
                      not r.startswith("/> cat filetwo") for r in rows))

            # --- and the window says where it stands --------------------
            # The window is maximised by now, its corner at (0, 0)
            titled = q.window_title(0, 0)
            check("the terminal window says which directory it stands in ('%s')"
                  % titled, titled.startswith("Terminal: /"))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
