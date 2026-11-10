#!/usr/bin/env python3
"""ThreadOS smoke test: the file manager.

What it replaced listed eight names in a window the size of a postage
stamp and showed the first hundred bytes of one of them. That proved
the path "disk -> click -> read -> drawing"; it was not a file
manager. A file manager is where a person GOES to find a file: it says
how big things are, it walks into directories and back out of them, it
hands a file to the program that opens it, and it throws one away when
asked twice.

The assertions follow exactly that. The sizes are checked against a
file whose length the test knows, because a column of numbers that
comes from nowhere proves nothing. Moving to Trash is checked in both
halves: the first click only ASKS, and the second moves the file where
the kernel's shell can independently find it. Restoring moves it back
to its original path. What the system keeps for itself
in the root (the swap file, the settings) stays out of the list until
the 'hidden' setting asks for it.

The window's name is checked too: it says which directory it is
showing, and changes when we walk into one. Two file managers open on
two directories would be two windows called "Files" otherwise.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, _kernel_define as ptest_define

# The window: slot 2 of the cascade, content a pixel right and a title
# bar down; gfiles asks for GFILES_W x GFILES_H (user/gfiles.c).
WX, WY = 94, 54
CX, CY = WX + 1, WY + CONTENT_DY
CW, CH = GFILES_W, GFILES_H
# Its three buttons, kept at the right edge of the heading
BTN_Y = GFILES_HEAD_H // 2
BTN_H = ptest_define("user/gfiles.c", "BTN_H")
DEL_W, EDIT_W, NAME_W = 70, 56, 70
DEL_X = CW - 10 - DEL_W
EDIT_X = DEL_X - 6 - EDIT_W
NAME_X = EDIT_X - 6 - NAME_W

# The question panel (user/ugui.h): a band across the middle of the
# window, with the two answers at its bottom edge
MODAL = ug_modal(CH)

TEXT = "Measurement results"          # what the test writes, and its length


def title(q):
    """The window's own name, as its title bar shows it."""
    return q.window_title(WX, WY)


def title_at(q, wx, wy):
    return q.window_title(wx, wy)


def rows(q):
    """(name, size) per row - read in the lettering the program writes in"""
    return [(n, sz) for n, sz, _ in q.gfiles_rows(CX, CY)]


def rows_at(q, wx, wy):
    return [(n, sz) for n, sz, _ in
            q.gfiles_rows(wx + 1, wy + CONTENT_DY)]


def row_y(names, name):
    return CY + gfiles_row_y(names.index(name))


def status(q):
    return q.gfiles_status(CX, CY)


def sudo_set(q, name, value, timeout=30):
    """'sudo set' in the kernel's shell, the password given if it is
    asked for - the signed-in administrator's shell does not ask.
    True once the shell says the setting was saved."""
    want = "%s = %s (saved)" % (name, value)
    start = len(q.serial())
    q.type_line("sudo set %s %s" % (name, value))
    asked = False
    end = time.time() + timeout
    while time.time() < end:
        out = q.serial()[start:]
        if want in out:
            return True
        if not asked and "password:" in out:
            q.type_line("thread")
            asked = True
        time.sleep(0.3)
    return False


def click(q, x, y, wait=1.5):
    q.mouse_goto(x, y)
    time.sleep(0.5)
    q.mouse_click()
    time.sleep(wait)


def ask_until(q, want, timeout=10):
    """The question the window is asking, read until it is 'want' or
    the time is up - a panel still sliding in reads as part of a line"""
    deadline = time.time() + timeout
    while True:
        asked = q.ug_question(CX, CY, CH)
        if asked == want or time.time() > deadline:
            return asked
        time.sleep(0.5)


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

            # The compositor is a kernel task that starts as Guest before
            # the profile is set up. The active account's Desktop must
            # remain readable after sign-in changes that task's identity.
            q.click_desktop()
            q.type_line("write /home/admin/Desktop/desk.txt visible")
            deadline = time.time() + 10
            desk_label = ""
            while time.time() < deadline:
                w, h, at = q.screen_raw()
                desk_label = q.find_ui(
                    270, 352,
                    ui_font_baseline_in(10, BAR_H + 8 + 43, 20),
                    charset="desk.txt", px=10,
                    under=lambda x, y: aurora_px(x, y, w, h),
                    screen=(w, h, at))
                if "desk.txt" in desk_label:
                    break
                time.sleep(0.25)
            check("a file in the signed-in user's Desktop appears as an icon "
                  "('%s')" % desk_label, "desk.txt" in desk_label)

            # Something to look at: a file whose length we know, and a
            # directory to walk into. The kernel's shell stands in the
            # signed-in user's home; the "Files" tile opens the root, so
            # that is where they go.
            q.command("cd /")
            q.command("write report.txt " + TEXT, "written", timeout=15)
            q.command("mkdir documents", "created", timeout=15)
            # A setting written down puts system.cfg beside swap.sys in
            # the root - two files the list should not offer
            check("the setting 'hidden 0' is saved", sudo_set(q, "hidden", "0"))
            on_disk = q.command("fls", "swap.sys", timeout=10)

            q.launch_dock("gfiles")                   # the "Files" tile
            time.sleep(4)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()

            listed = rows(q)
            names = [r[0] for r in listed]
            check("the manager lists what is on the disk (%s)"
                  % ", ".join(names), "report.txt" in names)
            check("and the directory too (%s)" % ", ".join(names),
                  "documents" in names)
            check("the kernel's shell lists the swap file and the settings",
                  "swap.sys" in on_disk and "system.cfg" in on_disk)
            check("but the file manager keeps them out of sight (%s)"
                  % ", ".join(names),
                  "swap.sys" not in names and "system.cfg" not in names)

            # The size column against a file whose length we know: the
            # shell writes the text and a newline
            size = dict(listed).get("report.txt", "")
            check("a file has its size beside it ('%s' next to %d characters "
                  "of text)" % (size, len(TEXT)),
                  size.startswith(str(len(TEXT))) or
                  size.startswith(str(len(TEXT) + 1)))
            check("and a directory has a word instead of a number ('%s')"
                  % dict(listed).get("documents", ""),
                  dict(listed).get("documents", "") == "folder")

            head = q.gfiles_heading(CX, CY)
            check("the heading says where we are ('%s')" % head,
                  head.startswith("/"))
            check("and the window is named after the directory ('%s')" % title(q),
                  title(q) == "Files: /")

            # --- walking into a directory -------------------------------
            click(q, CX + GFILES_ROW_X, row_y(names, "documents"))
            click(q, CX + EDIT_X + EDIT_W // 2, CY + BTN_Y, wait=2.5)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            head = q.gfiles_heading(CX, CY)
            check("'Open' on a directory goes into it ('%s')" % head,
                  head.startswith("/documents"))
            check("and the window says the same ('%s')" % title(q),
                  title(q) == "Files: /documents")
            inside = [n for n, _ in rows(q)]
            check("and it is empty inside but for the way back up (%s)"
                  % ", ".join(inside), inside in ([], [".."]))

            # --- and back out -------------------------------------------
            click(q, CX + 250, CY + 16, wait=1)   # the focus into the window
            q.sendkey("backspace")
            time.sleep(2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("Backspace goes back up ('%s')" % title(q),
                  title(q) == "Files: /")
            listed = rows(q)
            names = [r[0] for r in listed]
            check("and what was there shows again", "report.txt" in names)

            # --- moving to Trash asks first -----------------------------
            click(q, CX + GFILES_ROW_X, row_y(names, "report.txt"))
            click(q, CX + DEL_X + DEL_W // 2, CY + BTN_Y, wait=2)
            q.mouse_goto(500, 380)
            # The whole question, word for word: the name sits between
            # quotes the program writes itself, and one written twice
            # ("Move 'report.txt'' to ...") is a bug a part would hide
            asked = ask_until(q, "Move 'report.txt' to Trash?")
            check("a click on 'Trash' asks before moving the file ('%s')"
                  % asked, asked == "Move 'report.txt' to Trash?")

            # The question moves nothing, and "Keep" has to leave it
            # alone - a question whose answers both mean yes is not a
            # question. (While it is up, the list behind it is dimmed.)
            click(q, CX + MODAL["no"][0], CY + MODAL["no"][1], wait=2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("'Keep' keeps the file",
                  "report.txt" in [r[0] for r in rows(q)])

            click(q, CX + DEL_X + DEL_W // 2, CY + BTN_Y, wait=2)
            click(q, CX + MODAL["yes"][0], CY + MODAL["yes"][1], wait=2.5)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("confirming 'Move' sends it to Trash",
                  "report.txt" not in [r[0] for r in rows(q)])

            # Confirmed by something that is not this program
            q.mouse_goto(500, 380)
            q.mouse_click()                    # the desktop takes the keys
            time.sleep(1)
            seen = len(q.serial())
            q.type_line("cat report.txt")
            time.sleep(2)
            after = q.serial()[seen:]
            check("the original path is no longer readable",
                  TEXT not in after)

            trashed = q.command("cat /home/admin/.Trash/report.txt", TEXT,
                                timeout=15)
            check("and the file's data remains in the user's Trash",
                  TEXT in trashed)

            # A second file window opens Trash; Restore returns the entry
            # to the exact path recorded when it was moved. The shell
            # waits for what it runs, so it is free again only once this
            # window has closed.
            q.click_desktop()
            seen = len(q.serial())
            q.type_line("run gfiles /home/admin/.Trash")
            TX, TY = WX + 12, WY + 12
            end = time.time() + 20
            while (time.time() < end and
                   title_at(q, TX, TY) != "Files: /home/admin/.Trash"):
                time.sleep(0.5)
            got = title_at(q, TX, TY)
            check("Trash opens at its own path ('%s')" % got,
                  got == "Files: /home/admin/.Trash")
            listed = [name for name, _ in rows_at(q, TX, TY)]
            check("the removed item is listed in Trash (%s)"
                  % ", ".join(listed), "report.txt" in listed)
            # Restore acts on the item picked in the list, as every
            # button in the heading does
            TCX, TCY = TX + 1, TY + CONTENT_DY
            if "report.txt" in listed:
                click(q, TCX + GFILES_ROW_X,
                      TCY + gfiles_row_y(listed.index("report.txt")))
            click(q, TCX + NAME_X + NAME_W // 2, TCY + BTN_Y, wait=2)
            q.sendkey("alt-f4")                # Trash has the focus
            end = time.time() + 15
            while (time.time() < end and
                   "[gfiles] done" not in q.serial()[seen:]):
                time.sleep(0.3)
            check("the Trash window closes, and the shell is free again",
                  "[gfiles] done" in q.serial()[seen:])
            q.mouse_goto(600, 380)
            back = []
            end = time.time() + 10
            while time.time() < end and "report.txt" not in back:
                time.sleep(0.5)
                back = [name for name, _ in rows(q)]
            check("Restore puts it back in its original folder (%s)"
                  % ", ".join(back), "report.txt" in back)

            # --- a new name, asked for in the same status line ----------
            # The blocks do not move when a file is renamed, so this is
            # instant whatever its size - which is what the filesystem
            # grew a rename for.
            click(q, CX + 250, CY + 16, wait=1)
            names = [r[0] for r in rows(q)]
            click(q, CX + GFILES_ROW_X, row_y(names, "documents"))
            click(q, CX + NAME_X + NAME_W // 2, CY + BTN_Y, wait=2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            said = status(q)
            check("'Rename' asks for a new name ('%s')" % said,
                  "New name" in said)

            click(q, CX + 250, CY + 16, wait=1)   # the keys into the window
            q.type_line("papers", enter=False)
            time.sleep(0.5)
            q.sendkey("ret")
            time.sleep(2)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            names = [r[0] for r in rows(q)]
            check("and it changes it (%s)" % ", ".join(names),
                  "papers" in names and "documents" not in names)

            # --- an empty folder can be moved to Trash too --------------
            # (one with something in it too; what is refused before
            # asking is deleting such a folder from Trash for good -
            # scripts/tests/t_filetree.py)
            click(q, CX + 250, CY + 16, wait=1)
            names = [r[0] for r in rows(q)]
            click(q, CX + GFILES_ROW_X, row_y(names, "papers"))
            click(q, CX + DEL_X + DEL_W // 2, CY + BTN_Y, wait=2)
            q.mouse_goto(500, 380)
            asked = ask_until(q, "Move the folder 'papers' to Trash?")
            check("an empty folder is asked about too ('%s')" % asked,
                  asked == "Move the folder 'papers' to Trash?")
            click(q, CX + MODAL["yes"][0], CY + MODAL["yes"][1], wait=2.5)
            q.mouse_goto(500, 380)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            check("and the folder leaves its original location",
                  "papers" not in [r[0] for r in rows(q)])

            # --- the 'hidden' setting shows them ------------------------
            q.mouse_goto(500, 380)
            q.mouse_click()                    # the desktop takes the keys
            time.sleep(1)
            check("the setting 'hidden 1' is saved", sudo_set(q, "hidden", "1"))
            names = []
            end = time.time() + 15
            while time.time() < end and "swap.sys" not in names:
                time.sleep(1)
                names = [r[0] for r in rows(q)]
            check("with 'set hidden 1' the list shows them, without "
                  "reopening (%s)" % ", ".join(names),
                  "swap.sys" in names and "system.cfg" in names)

            q.click_desktop()
            q.command("write Files keep-this-name", "written", timeout=15)
            check("the setting 'language pl' is saved",
                  sudo_set(q, "language", "pl"))
            end = time.time() + 10
            while time.time() < end and title(q) != "Pliki: /":
                time.sleep(0.5)
            localized_rows = [name for name, _ in rows(q)]
            check("Polish localizes the file-manager window title",
                  title(q) == "Pliki: /")
            check("localized UI does not translate a file name",
                  "Files" in localized_rows)
            # The label is centred in its button, so it is looked for
            # across the button rather than read from its left edge
            shot = q.screen_raw()
            open_label = q.find_ui(
                CX + EDIT_X + 2, CX + EDIT_X + EDIT_W - 2,
                CY + ui_font_baseline_in(UD_PX, BTN_Y - BTN_H // 2, BTN_H),
                charset=UiFont.CHARSET + "ó", px=UD_PX,
                fg=theme("dark")["accent_text"], screen=shot)
            check("the file manager localizes its Open button ('%s')"
                  % open_label, open_label == "Otwórz")
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
