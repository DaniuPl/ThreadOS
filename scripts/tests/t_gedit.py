#!/usr/bin/env python3
"""ThreadOS smoke test: the text editor in a window.

The system could make files - 'write' at the prompt, 'echo' with a
'>' - but it could not CHANGE one. Everything that made a file was one
line typed at a prompt, and a line typed at a prompt cannot go back to
what it said a moment ago. That is the difference between a machine
that stores text and a machine somebody can work on.

So there is a page of white with black text on it, a caret that goes
where the arrows and the mouse say, Enter that splits a line and
Backspace that joins two, Ctrl+S to save and Ctrl+O to open. The
status line at the bottom is the whole of the interface: the file's
name, whether it has changed since it was saved, where the caret is -
and, when the editor needs a name, the question, asked in that same
line rather than in a second window to move, focus and close.

The assertions read the screen for the text and the status line, and
then step outside the editor entirely: the kernel's own shell types
'cat' at the file, so what is on the disk is checked by something that
was not the program that wrote it. Then the editor reads it back, the
clipboard carries a line from one place in the file to another, and
Backspace at the start of a line joins it to the one above - the only
way back from an Enter pressed by mistake.

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
from ptest import _kernel_define as ptest_define

# The window: slot 2 of the cascade, content a pixel right and a title
# bar down. Its size and the grid its letters sit on are the editor's
# own (user/gedit.c).
def _ed(name):
    return ptest_define("user/gedit.c", name)


WX, WY = 94, 54
CX, CY = WX + 1, WY + CONTENT_DY
CW, CH = _ed("W0"), _ed("H0")
ED_PAD, ED_ROW_H, ED_STATUS_H = _ed("PAD"), _ed("ROW_H"), _ed("STATUS_H")
ED_CELL, ED_PX, ED_BASE = _ed("CELL"), _ed("TEXT_PX"), _ed("TEXT_BASE")
ED_TEXT_X = _ed("TEXT_X")
PAD, ROW_H, STATUS_H = ED_PAD, ED_ROW_H, ED_STATUS_H
DARK = theme("dark")

# The panel that opens a file (user/ugui.h): a band across the middle
# of the window, a list of names in it, a line to type in and two
# buttons at the bottom edge.
PICK = ug_pick(CH)

# The terminal opens in the next slot of the cascade, behind the editor
TWX, TWY = 106, 66


def term_rows(q, rows=TERM_ROWS, cols=TERM_COLS):
    """The terminal's window, read in the lettering it is drawn in"""
    return q.term_rows(TWX + 1, TWY + CONTENT_DY, cols, rows)


def is_page(px):
    """The page: the theme's field"""
    return near(px, DARK["app_field"], 3)


def is_bar(px):
    """The status line along the bottom"""
    return near(px, DARK["app_row"], 3)


# The file manager: names at content (6,18), a row every ten pixels,
def files_listed(q, wx=WX, wy=WY):
    """The names on the file manager's list, in the lettering it writes
    them in"""
    return [r[0] for r in q.gfiles_rows(wx + 1, wy + CONTENT_DY)]


def files_click(q, wx, wy, x, y):
    """A click at (x, y) of the file manager's content"""
    q.mouse_goto(wx + 1 + x, wy + CONTENT_DY + y)
    time.sleep(0.5)
    q.mouse_click()


def row_text(at, r, cols=48, wx=WX, wy=WY):
    """One line of the page, read in the lettering it is drawn in: a grid
    of cells right of the line numbers (ptest.read_cells)"""
    return Qemu.read_cells(None, wx + 1 + ED_TEXT_X,
                           wy + CONTENT_DY + ED_PAD + r * ED_ROW_H, cols, 1,
                           cell=ED_CELL, row_h=ED_ROW_H, base=ED_BASE,
                           px=ED_PX, screen=(10000, 10000, at))[0]


def win_title(q, wx=WX, wy=WY):
    """The window's own name, as its title bar shows it."""
    return q.window_title(wx, wy)


def status_text(at, cols=48):
    """The status line along the bottom of the window"""
    base = CY + ui_font_baseline_in(11, CH - ED_STATUS_H, ED_STATUS_H)
    best = ""
    for face, fg in (("sans", DARK["app_text"]), ("bold", DARK["accent"])):
        got = Qemu.read_ui(None, CX + 10, base, CW - 20, px=11, face=face,
                           fg=fg, screen=(10000, 10000, at))
        if len(got) > len(best):
            best = got
    return best


def sixty_four():
    """The same editor on the 64-bit kernel. It is one source compiled
    twice, so what is asked here is the part that is not: that the
    calls it makes - a window, keys, and a file written to the disk -
    are all there in long mode too."""
    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("SKIPPED (64): no 64-bit image (make kernel64)")
        return
    if shutil.which("grub-mkrescue") is None or \
       shutil.which("qemu-system-x86_64") is None:
        print("SKIPPED (64): no grub-mkrescue or qemu-system-x86_64")
        return

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)

    try:
        os.makedirs(d + "/boot/grub")
        shutil.copy(KERNEL64, d + "/boot/threados64.bin")
        mod = ""
        if INITRD64 and os.path.exists(INITRD64):
            shutil.copy(INITRD64, d + "/boot/initrd64.img")
            mod = "    module2 /boot/initrd64.img initrd64\n"
        with open(d + "/boot/grub/grub.cfg", "w") as fh:
            fh.write("insmod all_video\nset gfxpayload=keep\n"
                     "set timeout=0\nset default=0\n"
                     'menuentry "ThreadOS 64" {\n'
                     "    multiboot2 /boot/threados64.bin\n" + mod + "}\n")
        made = subprocess.run(["grub-mkrescue", "-o", iso, d],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
        check("the disc with the 64-bit kernel builds", made)
        if not made:
            return

        q = Qemu(KERNEL, INITRD, disk=disk, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=60)
            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=30)
            q.wait_desktop(40)
            time.sleep(4)
            q.mouse_home()

            q.launch_dock("gedit")                # the "Editor" tile
            time.sleep(4)
            q.mouse_goto(CX + 200, CY + 100)
            q.mouse_click()                    # the focus, into the page
            time.sleep(1)

            q.type_line("two kernels", enter=False)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("(64) the editor writes on the page ('%s')" % row_text(at, 0),
                  row_text(at, 0) == "two kernels")

            # The same refusal, through the long-mode syscall of its own
            q.sendkey("alt-f4")
            time.sleep(3)
            w, h, at = q.screen_raw()
            check("(64) Alt+F4 on an unsaved file asks instead of closing "
                  "('%s')" % status_text(at),
                  "Unsaved changes" in status_text(at) and
                  row_text(at, 0) == "two kernels")

            q.sendkey("ctrl-s")
            time.sleep(1)
            q.type_line("long", enter=False)
            time.sleep(0.5)
            q.sendkey("ret")
            time.sleep(2)
            w, h, at = q.screen_raw()
            check("(64) and it saves under a name ('%s')" % status_text(at),
                  "long" in status_text(at) and "long*" not in status_text(at))

            # Checked by a program that is not the one that wrote the
            # file: the terminal, running the 64-bit 'cat'. In long
            # mode the kernel's own shell cannot be typed at from the
            # desktop - there is no console behind it - so the check
            # goes through the terminal window, which is a fuller
            # answer anyway: one application wrote the file and
            # another read it.
            q.launch_dock("gterm")                # the "Term" tile
            time.sleep(5)
            q.mouse_goto(TWX + 200, TWY + 100)
            q.mouse_click()
            time.sleep(1)
            q.type_line("ucat64 long")
            time.sleep(4)
            q.mouse_goto(600, 430)
            time.sleep(1)
            w, h, at = q.screen_raw()
            rows = term_rows(q)
            check("(64) another program reads off the disk what the editor wrote",
                  any("two kernels" in r for r in rows))

            # And the file manager hands a file to the editor here too -
            # in long mode that is a fork and an exec from a program
            # drawing in a window, which is its own piece of code
            fx, fy = 118, 78              # the next slot of the cascade
            q.launch_dock("gfiles")              # the "Files" tile
            time.sleep(5)
            q.mouse_goto(600, 430)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            listed = files_listed(q, fx, fy)
            row = listed.index("long") if "long" in listed else -1
            check("(64) the file manager sees the editor's file (%s)"
                  % ", ".join(n for n in listed if n), row >= 0)
            if row >= 0:
                files_click(q, fx, fy, GFILES_ROW_X, gfiles_row_y(row))
                time.sleep(1.5)
                files_click(q, fx, fy, *gfiles_button("open"))
                time.sleep(5)
                q.mouse_goto(600, 430)
                time.sleep(1.5)
                w, h, at = q.screen_raw()
                check("(64) 'Open' opens the file in the editor ('%s')"
                      % row_text(at, 0, wx=fx + 12, wy=fy + 12),
                      row_text(at, 0, wx=fx + 12, wy=fy + 12) == "two kernels")

                # And a new name for it - long mode has its own number
                # for that call and its own handler behind it.
                # The editor opened over the manager a moment ago, so
                # the manager has to come back to the top first - once
                # the editor really is there (it has the focus), or it
                # could open over the manager after the manager was
                # raised, and every click after that would land on it
                q.wait_focus(fx + 12, fy + 12, timeout=15)
                q.mouse_goto(fx + 100, fy + 4)
                time.sleep(0.5)
                q.mouse_click()
                q.mouse_goto(600, 430)
                q.wait_focus(fx, fy, timeout=10)
                files_click(q, fx, fy, GFILES_ROW_X, gfiles_row_y(row))
                time.sleep(1)
                files_click(q, fx, fy, *gfiles_button("rename"))
                time.sleep(1.5)
                q.mouse_goto(fx + 1 + 250, fy + CONTENT_DY + 16)
                time.sleep(0.5)
                q.mouse_click()                # the keys into the manager
                time.sleep(1)
                q.type_line("longer", enter=False)
                time.sleep(0.5)
                q.sendkey("ret")
                time.sleep(2.5)
                q.mouse_goto(600, 430)
                time.sleep(1.5)
                w, h, at = q.screen_raw()
                listed = files_listed(q, fx, fy)
                check("(64) 'Rename' changes the file's name (%s)"
                      % ", ".join(n for n in listed if n),
                      "longer" in listed and "long" not in listed)
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
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()

            q.launch_dock("gedit")                # the "Editor" tile
            time.sleep(4)
            q.mouse_goto(600, 430)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            check("the editor opened and is as white as a page",
                  is_page(at(CX + 200, CY + 100)))
            check("and it has a status bar at the bottom",
                  is_bar(at(CX + 200, CY + CH - 5)))

            # --- typing -------------------------------------------------
            q.type_line("The quick brown fox", enter=False)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("what was typed is on the page ('%s')" % row_text(at, 0),
                  row_text(at, 0) == "The quick brown fox")
            st = status_text(at)
            check("the status bar says the file has no name and has changed "
                  "('%s')" % st, "(no name)*" in st)
            check("and where the cursor stands (line 1, column 20)",
                  "L1 C20" in st)

            q.sendkey("ret")
            q.type_line("jumps over", enter=False)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("Enter starts a new line",
                  row_text(at, 0) == "The quick brown fox" and
                  row_text(at, 1) == "jumps over")
            check("and the status bar counts the lines", "L2 C11" in status_text(at))

            # --- Ctrl+F: finding, and going round the end ---------------
            q.sendkey("ctrl-f")
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("Ctrl+F asks what to look for ('%s')" % status_text(at),
                  "Find:" in status_text(at))
            q.type_line("fox", enter=False)
            time.sleep(0.5)
            q.sendkey("ret")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            st = status_text(at)
            # The caret was on line two, past the word - so finding it
            # at all means the search went round the end of the file
            check("it finds a word ABOVE the cursor, so it wraps around "
                  "the end of the file ('%s')" % st,
                  "found on line 1" in st and "L1 C17" in st)

            q.sendkey("ctrl-f")
            time.sleep(0.8)
            q.sendkey("ret")                   # nothing typed = the same again
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("Enter on an empty question repeats the last search "
                  "('%s')" % status_text(at),
                  "found" in status_text(at))

            q.sendkey("ctrl-f")
            time.sleep(0.8)
            q.type_line("nosuchthing", enter=False)
            time.sleep(0.5)
            q.sendkey("ret")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("and what is not there it does not find ('%s')"
                  % status_text(at), "not found" in status_text(at))

            # --- Ctrl+Z: taking it back ---------------------------------
            q.sendkey("down")
            q.sendkey("end")
            time.sleep(0.5)
            q.sendkey("ctrl-z")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("Ctrl+Z undoes the last character ('%s')" % row_text(at, 1),
                  row_text(at, 1) == "jumps ove")
            check("and it says it undid something", "undone" in status_text(at))

            q.sendkey("ctrl-z")
            q.sendkey("ctrl-z")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("and it keeps undoing, one at a time ('%s')" % row_text(at, 1),
                  row_text(at, 1) == "jumps o")

            # Back to what it was - the checks below expect it
            q.type_line("ver", enter=False)
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("and what was typed after the undo is in its place",
                  row_text(at, 1) == "jumps over")

            # --- saving -------------------------------------------------
            q.sendkey("ctrl-s")
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("Ctrl+S asks for a name in the status bar",
                  "Save as:" in status_text(at))
            q.type_line("note", enter=False)
            time.sleep(0.5)
            q.sendkey("ret")
            time.sleep(2)
            w, h, at = q.screen_raw()
            st = status_text(at)
            check("after the save the bar knows the name and the star is gone "
                  "('%s')" % st, "note" in st and "note*" not in st)
            # And the window says which file it holds - three editors
            # open on three files would be three windows called
            # "Editor" otherwise
            check("the window is named after the file ('%s')" % win_title(q),
                  win_title(q) == "Editor: note")

            # --- and the file is really on the disk ---------------------
            # Checked by something that is not the program that wrote
            # it: the kernel's shell, which listens when no window has
            # the focus.
            q.click_desktop()
            time.sleep(1)
            q.type_line("cat note")
            time.sleep(2)
            log = q.serial()
            check("the file is on the disk and holds what was written",
                  "The quick brown fox" in log and "jumps over" in log)

            # --- reading it back ----------------------------------------
            q.mouse_goto(CX + 200, CY + 100)   # back into the editor
            q.mouse_click()
            time.sleep(1)
            q.type_line("rubbish", enter=False)
            time.sleep(1)
            q.sendkey("ctrl-o")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            # The panel shows what is on the disk: a name no longer has
            # to be remembered to be opened.
            title, listed = q.ug_pick_read(CX, CY, CH)
            check("Ctrl+O shows what is on the disk ('%s': %s)"
                  % (title.strip(), ", ".join(x for x in listed if x)),
                  "Open a file" in title and
                  any("note" in x for x in listed))

            # Typing still works, and is still the way to name a file
            # that is not on the list
            q.type_line("note", enter=False)
            time.sleep(0.5)
            q.sendkey("ret")
            time.sleep(2)
            w, h, at = q.screen_raw()
            check("the opened file replaced what was in the window",
                  row_text(at, 0) == "The quick brown fox" and
                  row_text(at, 1) == "jumps over" and
                  row_text(at, 2) == "")

            # And the new way: click a name, click "Open"
            q.sendkey("ctrl-o")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            listed = q.ug_pick_read(CX, CY, CH)[1]
            which = listed.index("note") if "note" in listed else -1
            check("a name from the disk is on the list to be clicked", which >= 0)
            if which >= 0:
                q.mouse_goto(CX + 60, CY + PICK["row_y"](which))
                q.mouse_click()
                time.sleep(1)
                q.mouse_goto(CX + PICK["yes"][0], CY + PICK["yes"][1])
                q.mouse_click()
                time.sleep(2)
                # 600x430 and not further: this screen is 640x480, and
                # a pointer moved past its edge is clamped by the
                # driver while the test goes on thinking it is where it
                # asked for - after which every click lands somewhere
                # else. (That cost an afternoon.)
                q.mouse_goto(600, 430)
                time.sleep(1)
                w, h, at = q.screen_raw()
                check("a click on the name and on 'Open' opens the same file",
                      row_text(at, 0) == "The quick brown fox")

            # --- the clipboard, shared with the rest of the desktop -----
            q.sendkey("ctrl-insert")           # copies the caret's line
            time.sleep(1)
            q.sendkey("down")
            q.sendkey("down")
            q.sendkey("end")
            q.sendkey("ret")
            time.sleep(0.5)
            q.sendkey("shift-insert")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("Ctrl+Insert and Shift+Insert move a line "
                  "('%s')" % row_text(at, 2),
                  row_text(at, 2) == "The quick brown fox")

            # --- backspace joins two lines ------------------------------
            q.sendkey("home")
            time.sleep(0.5)
            q.sendkey("backspace")
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("Backspace at the start of a line joins it to the one before "
                  "('%s')" % row_text(at, 1),
                  row_text(at, 1) == "jumps overThe quick brown fox" and
                  row_text(at, 2) == "")

            # --- closing is a request, and it can be declined -----------
            # A file with changes in it is not something to lose
            # because a window was closed. The editor answers the first
            # request with "not yet" - the desktop stops counting down
            # - and turns it into a question; the second close is the
            # answer to that question.
            q.type_line("change", enter=False)
            # The pointer away from the page: the cursor is drawn white,
            # and left on the page it is indistinguishable from the page
            # - a probe would then say "the window is still there" about
            # a window that had gone.
            q.mouse_goto(600, 200)
            time.sleep(1.5)
            open_before = q.dock_windows()
            q.sendkey("alt-f4")
            time.sleep(3)
            w, h, at = q.screen_raw()
            st = status_text(at)
            check("Alt+F4 on an unsaved file does NOT close, it asks "
                  "('%s')" % st, "Unsaved changes" in st)
            check("and the window stands where it stood",
                  is_page(at(CX + 200, CY + 100)))
            # And the desktop did stop counting: two seconds is the
            # whole of the grace, so a window still here five seconds
            # later was not merely slow to die
            time.sleep(5)
            w, h, at = q.screen_raw()
            check("the desktop stopped counting - after five seconds the window "
                  "is still there", is_page(at(CX + 200, CY + 100)))

            q.sendkey("alt-f4")
            left = open_before
            for _ in range(10):                # waiting, not asserting
                time.sleep(1)
                left = q.dock_windows()
                if left < open_before:
                    break
            check("a second Alt+F4 closes it (%d -> %d windows in the dock)"
                  % (open_before, left), left == open_before - 1)
            check("and the program ended by itself, it was not killed",
                  "process \'gedit\' has ended (code 0)" in q.serial())

            # --- a file named when the editor is started ----------------
            # 'gedit notatka' opens that file at once. Making a person
            # type the name again, in the program they have just told
            # which file they mean, is what makes an editor a chore.
            q.click_desktop()
            time.sleep(1)
            q.type_line("run gedit note")
            time.sleep(4)
            q.mouse_goto(600, 430)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("'gedit note' opens the file at once ('%s')"
                  % row_text(at, 0),
                  row_text(at, 0) == "The quick brown fox" and
                  row_text(at, 1) == "jumps over")

            # --- and the file manager can hand a file over --------------
            # What a file manager is FOR is getting to a file; getting
            # to a text file means opening it in the editor.
            q.sendkey("alt-f4")
            time.sleep(2.5)
            # The editor is closed; launch the file manager.
            q.launch_dock("gfiles", n_w=0)            # the "Files" tile
            time.sleep(4)
            q.mouse_goto(600, 430)
            time.sleep(1.5)
            w, h, at = q.screen_raw()

            listed = files_listed(q)
            row = listed.index("note") if "note" in listed else -1
            check("the file manager sees the saved file (%s)"
                  % ", ".join(n for n in listed if n), row >= 0)
            if row >= 0:
                files_click(q, WX, WY, GFILES_ROW_X, gfiles_row_y(row))
                time.sleep(1.5)
                files_click(q, WX, WY, *gfiles_button("open"))
                time.sleep(4)
                q.mouse_goto(600, 430)
                time.sleep(1.5)
                w, h, at = q.screen_raw()
                # The new window is the next slot of the cascade
                check("the 'Open' button opens the file in the editor ('%s')"
                      % row_text(at, 0, wx=WX + 12, wy=WY + 12),
                      row_text(at, 0, wx=WX + 12, wy=WY + 12) ==
                      "The quick brown fox")
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    print("--- the same editor on the 64-bit kernel ---")
    sixty_four()
    return summary()


sys.exit(main())
