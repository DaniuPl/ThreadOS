#!/usr/bin/env python3
"""ThreadOS smoke test: the file manager's folder tree, new folders,
cut, copy and paste, and the double click.

What a person expects of a file manager after using any other one: the
folders as a tree down the left, a folder made from where one is, a
file copied or moved from one folder into another, and a double click
that opens. user/gfiles.c builds the tree by walking - into each open
folder, one readdir each - because the kernel reads only the directory
a program stands in.

From outside:

  the tree shows the disk and the folders in it, and the one being
  shown is marked; a click on a folder in the tree goes there, and the
  folder opens to show its own; its arrow closes it again

  'New folder' asks for a name in the status line and makes it - the
  kernel's shell then lists it

  'Copy' and 'Paste' in the same folder make a copy with a name of its
  own, as long as the original; 'Cut', a folder in the tree and 'Paste'
  move it, and it is gone from where it was

  a double click on a folder walks into it; a file copied into it
  there is read back by the kernel's own 'cat', byte for byte

  a folder with something in it goes to Trash, with what is in it,
  once the question has been answered; in Trash it is not removed for
  good while something is still in it, and the program says why before
  asking anything

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

# The window: slot 2 of the cascade, content a pixel right and a title
# bar down; gfiles asks for GFILES_W x GFILES_H (user/gfiles.c).
WX, WY = 94, 54
CX, CY = WX + 1, WY + CONTENT_DY
AWAY = (500, 380)                     # the pointer off the window
QUIET = (CX + 250, CY + 16)           # an empty piece of the heading

TEXT = "Measurement results"


def click(q, x, y, wait=1.5):
    q.mouse_goto(x, y)
    time.sleep(0.5)
    q.mouse_click()
    time.sleep(wait)


def double(q, x, y, wait=2.5):
    q.mouse_goto(x, y)
    time.sleep(0.5)
    q.mouse_click()
    q.mouse_click()
    time.sleep(wait)


def look(q):
    q.mouse_goto(*AWAY)
    time.sleep(1.5)


def rows(q):
    return [(n, sz) for n, sz, _ in q.gfiles_rows(CX, CY)]


def names(q):
    return [n for n, _ in rows(q)]


def tree(q):
    return q.gfiles_tree(CX, CY)


def tool(q, which, wait=1.5):
    x, y = gfiles_tool(which)
    click(q, CX + x, CY + y, wait)


def pick(q, name):
    listed = names(q)
    click(q, CX + GFILES_ROW_X, CY + gfiles_row_y(listed.index(name)))


def tree_click(q, name, arrow=False):
    """A click on a folder in the tree: on its name, or on its arrow"""
    lines = tree(q)
    for i, (depth, n, _) in enumerate(lines):
        if n == name:
            x = 8 + depth * GFILES_TREE_STEP + (3 if arrow else 34)
            click(q, CX + x, CY + gfiles_tree_y(i), wait=2)
            return True
    return False


def typed(q, text):
    click(q, *QUIET, wait=1)          # the keys into the window
    q.type_line(text, enter=False)
    time.sleep(0.5)
    q.sendkey("ret")
    time.sleep(2)


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

            # A file whose length we know, a folder with a folder and a
            # file in it - made in the root, where the "Files" tile
            # opens; the kernel's shell starts in the user's home. The
            # folders and the copy are made by 'core', in ring 3.
            q.command("cd /")
            q.command("write report.txt " + TEXT, "written", timeout=15)
            for line in ("run core mkdir documents",
                         "run core mkdir documents/inner",
                         "run core cp report.txt documents/inside.txt"):
                said = q.command(line, "[core] done", timeout=20)
                check("'%s' succeeds" % line[4:],
                      "[core] done, exit code 0" in said)

            q.launch_dock("gfiles")                   # the "Files" tile
            time.sleep(4)
            look(q)

            # --- the tree -----------------------------------------------
            lines = tree(q)
            check("the tree shows the disk, marked as the folder shown (%s)"
                  % lines, lines[:1] == [(0, "Disk", True)])
            check("and the folder on it, one step in (%s)" % lines,
                  (1, "documents", False) in lines and
                  not any(n == "inner" for _, n, _ in lines))

            tree_click(q, "documents")
            look(q)
            lines = tree(q)
            check("a click on a folder in the tree goes there ('%s')"
                  % q.window_title(WX, WY),
                  q.window_title(WX, WY) == "Files: /documents")
            check("and it is marked, and opens to show its own folder (%s)"
                  % lines, (1, "documents", True) in lines and
                  (2, "inner", False) in lines)
            check("the list shows what is in it (%s)" % names(q),
                  sorted(names(q)) == ["inner", "inside.txt"])

            tree_click(q, "documents", arrow=True)
            look(q)
            lines = tree(q)
            check("its arrow closes it, and the list stays where it is (%s)"
                  % lines, (1, "documents", True) in lines and
                  not any(n == "inner" for _, n, _ in lines) and
                  q.window_title(WX, WY) == "Files: /documents")

            tree_click(q, "Disk")
            look(q)
            check("and the disk in the tree goes back to the top ('%s')"
                  % q.window_title(WX, WY),
                  q.window_title(WX, WY) == "Files: /")

            # --- a new folder -------------------------------------------
            tool(q, "new")
            look(q)
            said = q.gfiles_status(CX, CY)
            check("'New folder' asks for its name ('%s')" % said,
                  "Folder name" in said)
            typed(q, "archive")
            look(q)
            listed = dict(rows(q))
            check("and makes it (%s)" % ", ".join(listed),
                  listed.get("archive") == "folder")
            check("the tree has it too (%s)" % tree(q),
                  (1, "archive", False) in tree(q))

            # --- copy and paste in the same folder ----------------------
            pick(q, "report.txt")
            tool(q, "copy")
            look(q)
            clip = q.gfiles_clipboard(CX, CY)
            check("'Copy' says what it holds ('%s')" % clip,
                  clip == "copied: report.txt")
            tool(q, "paste", wait=2.5)
            look(q)
            listed = dict(rows(q))
            check("'Paste' beside the original makes a copy with a name of "
                  "its own (%s)" % ", ".join(listed),
                  "report-copy.txt" in listed and "report.txt" in listed)
            check("as long as the original ('%s' and '%s')"
                  % (listed.get("report-copy.txt"), listed.get("report.txt")),
                  listed.get("report-copy.txt") == listed.get("report.txt")
                  and listed.get("report.txt"))

            # --- cut, a folder in the tree, paste -----------------------
            pick(q, "report-copy.txt")
            tool(q, "cut")
            tree_click(q, "archive")
            look(q)
            check("the tree takes one into the folder to paste into ('%s')"
                  % q.window_title(WX, WY),
                  q.window_title(WX, WY) == "Files: /archive")
            tool(q, "paste", wait=2.5)
            look(q)
            check("'Paste' after 'Cut' moves the file here (%s)" % names(q),
                  names(q) == ["report-copy.txt"])
            tree_click(q, "Disk")
            look(q)
            check("and it is gone from where it was (%s)" % names(q),
                  "report-copy.txt" not in names(q) and
                  "report.txt" in names(q))

            # --- a double click, and a copy into another folder ---------
            pick(q, "report.txt")
            tool(q, "copy")
            listed = names(q)
            double(q, CX + GFILES_ROW_X, CY + gfiles_row_y(listed.index("archive")))
            look(q)
            check("a double click on a folder walks into it ('%s')"
                  % q.window_title(WX, WY),
                  q.window_title(WX, WY) == "Files: /archive")
            tool(q, "paste", wait=2.5)
            look(q)
            check("a copy pasted in another folder keeps its name (%s)"
                  % names(q),
                  sorted(names(q)) == ["report-copy.txt", "report.txt"])

            # --- a folder with something in it --------------------------
            # Outside Trash it goes there like anything else, once asked
            # - Trash is where it can be had back from
            tree_click(q, "Disk")
            look(q)
            pick(q, "documents")
            x, y = gfiles_button("remove")
            click(q, CX + x, CY + y, wait=2)
            look(q)
            # The whole question: it is put together from pieces, and a
            # piece too many (a quote twice) still holds both words
            want = "Move the folder 'documents' to Trash?"
            asked = q.ug_question(CX, CY, GFILES_H)
            end = time.time() + 10
            while asked != want and time.time() < end:
                time.sleep(0.5)
                asked = q.ug_question(CX, CY, GFILES_H)
            check("a folder with something in it is asked about before it "
                  "goes to Trash ('%s')" % asked, asked == want)
            yes = ug_modal(GFILES_H)["yes"]
            click(q, CX + yes[0], CY + yes[1], wait=2.5)
            look(q)
            check("and 'Move' takes it from where it was (%s)" % names(q),
                  "documents" not in names(q) and "archive" in names(q))

            # Trash in a window of its own. The shell waits for what it
            # runs, so it is free again only once that window is closed.
            q.mouse_goto(*AWAY)
            q.mouse_click()                    # the desktop takes the keys
            time.sleep(1)
            seen = len(q.serial())
            q.type_line("run gfiles /home/admin/.Trash")
            tx, ty = WX + 1 + 12, WY + CONTENT_DY + 12
            trash = []
            end = time.time() + 20
            while time.time() < end and "documents" not in trash:
                time.sleep(0.5)
                trash = [n for n, _, _ in q.gfiles_rows(tx, ty)]
            check("Trash holds the folder (%s)" % ", ".join(trash),
                  "documents" in trash)
            if "documents" in trash:
                click(q, tx + GFILES_ROW_X,
                      ty + gfiles_row_y(trash.index("documents")))
            click(q, tx + x, ty + y, wait=2)
            look(q)
            said = q.gfiles_status(tx, ty)
            check("a folder with something in it is not removed, and it "
                  "says why ('%s')" % said, "not empty" in said)
            check("and it is still there",
                  "documents" in [n for n, _, _ in q.gfiles_rows(tx, ty)])
            q.sendkey("alt-f4")                # Trash has the focus
            end = time.time() + 15
            while (time.time() < end and
                   "[gfiles] done" not in q.serial()[seen:]):
                time.sleep(0.3)
            check("the Trash window closes, and the shell is free again",
                  "[gfiles] done" in q.serial()[seen:])

            # --- confirmed by something that is not this program --------
            q.mouse_goto(*AWAY)
            q.mouse_click()                    # the desktop takes the keys
            time.sleep(1)
            seen = len(q.serial())
            q.type_line("cat archive/report.txt")
            time.sleep(2)
            q.type_line("fls")
            time.sleep(2)
            after = q.serial()[seen:]
            check("the kernel's shell reads the copy in the new folder",
                  TEXT in after)
            check("and lists the folder made in the window",
                  "archive" in after)
            kept = q.command("cat /home/admin/.Trash/documents/inside.txt",
                             TEXT, timeout=15)
            check("and what was in the folder went to Trash with it",
                  TEXT in kept)
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
