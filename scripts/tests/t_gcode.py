#!/usr/bin/env python3
"""ThreadOS smoke test: Code, the development environment.

Code (user64/gcode.c) is laid out the way the JetBrains IDEs are: a
toolbar with the project and Run, tool windows on a stripe at the left -
Project and Structure above, Run, Terminal and Problems below - tabs
over the editor and a status bar. The test works through it the way a
person would, reading everything back from the screen:

  with no project yet it welcomes; New Project makes ~/Projects/hello
  from the JavaScript template - the files are on the disk, the tree
  lists them, main.js is open with its text coloured: keywords, strings
  and comments each their own colour

  typing: a block opened with { indents the next line and a } closes it
  again, a bracket typed is closed and typing the closing one steps over
  it; Ctrl+/ comments a line out and back in; Shift and the arrows
  select, and what is typed replaces the selection; Ctrl+D doubles a
  line and Ctrl+Z takes back one step at a time; Ctrl+S saves, and the
  disk has what the screen shows

  Run saves and runs the project ('js main.js'): what it prints, and
  that it finished with code 0; an error it reports names main.js and a
  line - Problems lists it, and a click there takes the caret to it

  the + in the Project window makes a file, which opens in a tab of its
  own; Ctrl+O goes to a file by a part of its name; Structure lists the
  file's functions and a click goes to one; the Terminal runs a shell
  in the project's folder (whose .code directory the tree and Go to
  File leave out)

  closed and started again, Code comes back to the project with its
  file open; Close Project goes back to the welcome, which lists it

Run on its own or through scripts/run_tests.py.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, _kernel_define as define


def ide(name):
    return define("user64/gcode.c", name)


W0, H0 = ide("W0"), ide("H0")
BAR_H, STRIPE_W, STRIPE_STEP = ide("BAR_H"), ide("STRIPE_W"), ide("STRIPE_STEP")
STRIPE_ICON, SIDE_W, HEAD_H = ide("STRIPE_ICON"), ide("SIDE_W"), ide("HEAD_H")
TREE_ROW_H, TREE_INDENT, PLUS_R = (ide("TREE_ROW_H"), ide("TREE_INDENT"),
                                   ide("PLUS_R"))
TAB_H, GUTTER_W, CODE_PAD = ide("TAB_H"), ide("GUTTER_W"), ide("CODE_PAD")
FIND_H = ide("FIND_H")
CELL, ROW_H, TEXT_PX, TEXT_BASE = (ide("CELL"), ide("ROW_H"), ide("TEXT_PX"),
                                   ide("TEXT_BASE"))
BOTTOM_H, STATUS_H, UI_PX = ide("BOTTOM_H"), ide("STATUS_H"), ide("UI_PX")
BTN, RUN_R, CFG_W = ide("BTN"), ide("RUN_R"), ide("CFG_W")
POP_W, POP_Y, POP_TITLE_H, POP_IN_H, POP_ROW_H = (
    ide("POP_W"), ide("POP_Y"), ide("POP_TITLE_H"), ide("POP_IN_H"),
    ide("POP_ROW_H"))
WEL_SIDE_W, WEL_HEAD_H, WEL_ROW_H = (ide("WEL_SIDE_W"), ide("WEL_HEAD_H"),
                                     ide("WEL_ROW_H"))
WEL_BTN_W, WEL_BTN_H = ide("WEL_BTN_W"), ide("WEL_BTN_H")
TPL_TOP, TPL_ROW_H = ide("TPL_TOP"), ide("TPL_ROW_H")

# The dark New UI palette (dark_pal in gcode.c)
UI = (0xDF, 0xE1, 0xE5)
DIM = (0x6F, 0x73, 0x7A)
KEYWORD = (0xCF, 0x8E, 0x6D)
STRING = (0x6A, 0xAB, 0x73)
COMMENT = (0x7A, 0x7E, 0x85)
DIRECTIVE = (0xB3, 0xAE, 0x60)
FUNCTION = (0x56, 0xA8, 0xF5)

ED_X = STRIPE_W + SIDE_W + 1          # the editor, with Project open
CODE_X = ED_X + GUTTER_W + CODE_PAD
CODE_Y = BAR_H + TAB_H
BOT_Y = H0 - STATUS_H - BOTTOM_H
CX = CY = 0


def find_code(q, timeout=15):
    """The window, wherever the desktop put it"""
    global CX, CY
    width, height = q.size()
    frame_w, frame_h = W0 + 2, H0 + TITLE_H + 2
    bottom = work_bottom(h=height)
    spots = []
    for slot in range(8):
        wx, wy = cascade(slot, frame_w, frame_h, w=width, h=height)
        if wx + frame_w > width:
            wx = max(0, width - frame_w)
        if wy + frame_h > bottom:
            wy = max(0, bottom - frame_h)
        if (wx, wy) not in spots:
            spots.append((wx, wy))
    end = time.time() + timeout
    while time.time() < end:
        for wx, wy in spots:
            if q.window_title(wx, wy, W0 + 2) in ("Code", "Kod"):
                CX, CY = wx + 1, wy + CONTENT_DY
                return True
        time.sleep(0.25)
    return False


def poll(fn, want, timeout=10):
    """fn() until want(result) holds, or the time is up; the last result"""
    end = time.time() + timeout
    got = fn()
    while not want(got) and time.time() < end:
        time.sleep(0.3)
        got = fn()
    return got


def away(q):
    w, h = q.size()
    q.mouse_goto(w - 6, h // 2)


def click(q, x, y):
    q.click(CX + x, CY + y)
    away(q)


def code_rows(q, first=0, n=1, screen=None):
    cols = (W0 - CODE_X - 8) // CELL
    return q.read_cells(CX + CODE_X, CY + CODE_Y + first * ROW_H, cols, n,
                        cell=CELL, row_h=ROW_H, base=TEXT_BASE, px=TEXT_PX,
                        screen=screen)


def code_row(q, row):
    return code_rows(q, row)[0]


def bottom_rows(q, n):
    cols = (W0 - ED_X - 20) // CELL
    return q.read_cells(CX + ED_X + 10, CY + BOT_Y + HEAD_H, cols, n,
                        cell=CELL, row_h=ROW_H, base=TEXT_BASE, px=TEXT_PX)


def ui_text(q, x, top, h, width, fg=UI, px=UI_PX, face="sans", screen=None):
    return q.read_ui(CX + x, CY + ui_font_baseline_in(px, top, h), width,
                     px=px, face=face, fg=fg, screen=screen)


def tree(q, rows=6):
    shot = q.screen_raw()
    out = []
    for r in range(rows):
        top = BAR_H + HEAD_H + r * TREE_ROW_H
        names = [ui_text(q, STRIPE_W + 10 + d * TREE_INDENT + 36, top,
                         TREE_ROW_H, SIDE_W - 60, screen=shot)
                 for d in range(2)]
        out.append(max(names, key=len))
    return [n for n in out if n]


def status(q):
    """The left of the status bar: a message, or where the file is"""
    top = H0 - STATUS_H
    return q.find_ui(CX + 8, CX + W0 // 2,
                     CY + ui_font_baseline_in(11, top, STATUS_H), px=11,
                     colours=[UI, DIM])


def status_right(q):
    top = H0 - STATUS_H
    return q.find_ui(CX + W0 // 2, CX + W0 - 4,
                     CY + ui_font_baseline_in(11, top, STATUS_H), px=11,
                     fg=DIM)


def stripe_bottom(j):
    return (STRIPE_W // 2, H0 - STATUS_H - 6 - (3 - j) * STRIPE_STEP +
            (STRIPE_STEP - STRIPE_ICON) + STRIPE_ICON // 2)


def stripe_top(i):
    return STRIPE_W // 2, BAR_H + 6 + i * STRIPE_STEP + STRIPE_ICON // 2


def ink_of(q, row, col0, col1, screen=None):
    """The colour of the text in some cells of a line of the editor: its
    inkiest pixel, the one furthest from the ground - a stroke that
    covers a pixel wholly is exactly the colour it was drawn in"""
    _, _, at = screen or q.screen_raw()
    y0 = CY + CODE_Y + row * ROW_H
    px = Counter(at(CX + CODE_X + c * CELL + dx, y0 + dy)
                 for c in range(col0, col1) for dx in range(CELL - 1)
                 for dy in range(ROW_H - 2))
    ground = px.most_common(1)[0][0]
    far = [(sum(abs(a - b) for a, b in zip(c, ground)), c) for c in px
           if c != ground]
    return max(far)[1] if far else None


def same(c, want):
    """A text colour read off the screen: a short word may have no pixel
    its strokes cover wholly, so its inkiest is a shade short of it - but
    nowhere near the colour of another kind of word"""
    return c is not None and all(abs(a - b) <= 6 for a, b in zip(c, want))


def disk_file(q, path, until):
    """A file as the kernel shell's cat prints it, as far as 'until'"""
    q.click_desktop()
    return q.command("cat %s" % path, until, timeout=15)


def welcome(q):
    mid = WEL_SIDE_W + (W0 - WEL_SIDE_W) // 2
    return q.find_ui(CX + mid - 150, CX + mid + 150,
                     CY + H0 // 2 - 10, px=18, face="bold", fg=UI)


def open_code(q):
    q.launch_dock("gcode")
    check("Code opens from the dock", find_code(q))
    away(q)
    said = poll(lambda: welcome(q), lambda s: s == "Welcome to Code")
    check("with no project yet it welcomes ('%s')" % said,
          said == "Welcome to Code")


def test_ide(q):
    open_code(q)

    # New Project, from the JavaScript template
    click(q, W0 - 20 - 2 * WEL_BTN_W - 10 + WEL_BTN_W // 2,
          (WEL_HEAD_H - WEL_BTN_H) // 2 + WEL_BTN_H // 2)
    time.sleep(1)
    q.type_line("hello", enter=False)
    click(q, 60, TPL_TOP + TPL_ROW_H + TPL_ROW_H // 2)
    time.sleep(0.5)
    q.sendkey("ret")
    name = poll(lambda: ui_text(q, 42, 0, BAR_H, 200, face="bold"),
                lambda s: s == "hello")
    check("New Project opens it: its name in the toolbar ('%s')" % name,
          name == "hello")
    files = poll(lambda: tree(q), lambda t: t == ["main.js", "README.md"])
    check("the tree lists the template's files (%s)" % files,
          files == ["main.js", "README.md"])
    rows = poll(lambda: code_rows(q, 0, 7),
                lambda r: r[2] == "function greet(name) {")
    check("main.js is open with the template's text (%s)" % rows[:3],
          rows[0] == "// hello" and rows[2] == "function greet(name) {" and
          rows[3] == '    return "Hello, " + name + "!";')

    shot = q.screen_raw()
    kw = ink_of(q, 2, 0, 8, shot)          # function
    plain = ink_of(q, 2, 14, 18, shot)     # name
    st = ink_of(q, 3, 11, 19, shot)        # "Hello,
    com = ink_of(q, 0, 0, 8, shot)         # // hello
    check("keywords, strings and comments each their colour (%s %s %s %s)"
          % (kw, st, com, plain),
          same(kw, KEYWORD) and same(st, STRING) and same(com, COMMENT) and
          not any(same(plain, c) for c in (KEYWORD, STRING, COMMENT)))

    # Typing a function: braces, indent, brackets
    click(q, CODE_X + 40, CODE_Y + 6 * ROW_H + 6)
    q.sendkey("ctrl-end")
    q.sendkey("ret")
    q.type_line("function twice(n) {", enter=True)
    q.type_line("return n * 2;", enter=True)
    q.type_line("}", enter=True)
    q.type_line("console.log(twice(21));", enter=False)
    want = ["function twice(n) {", "    return n * 2;", "}",
            "console.log(twice(21));"]
    rows = poll(lambda: code_rows(q, 7, 4), lambda r: r == want)
    check("a block indents the line after { and } closes it; a bracket "
          "typed is closed and stepped over (%s)" % rows, rows == want)

    # Ctrl+/ on 'return n * 2;' - and back
    q.sendkey("up")
    q.sendkey("up")
    q.sendkey("ctrl-slash")
    got = poll(lambda: code_row(q, 8), lambda r: r == "    // return n * 2;")
    q.sendkey("up")
    q.sendkey("ctrl-slash")
    back = poll(lambda: code_row(q, 8), lambda r: r == "    return n * 2;")
    check("Ctrl+/ comments a line out ('%s') and back in ('%s')"
          % (got, back),
          got == "    // return n * 2;" and back == "    return n * 2;")

    # Shift and the arrows select; typing replaces the selection
    q.sendkey("down")                          # console.log(twice(21));
    q.sendkey("end")
    for _ in range(5):
        q.sendkey("shift-left")
    q.type_line("10));", enter=False)
    got = poll(lambda: code_row(q, 10), lambda r: r == "console.log(twice(10));")
    check("Shift with the arrows selects, and typing replaces it ('%s')"
          % got, got == "console.log(twice(10));")

    # Ctrl+D doubles the line; Ctrl+Z takes it back, then the typing
    q.sendkey("ctrl-d")
    two = poll(lambda: code_rows(q, 10, 2),
               lambda r: r == ["console.log(twice(10));"] * 2)
    q.sendkey("ctrl-z")
    one = poll(lambda: code_rows(q, 10, 2), lambda r: r[1] == "")
    q.sendkey("ctrl-z")
    undone = poll(lambda: code_row(q, 10),
                  lambda r: r == "console.log(twice(21));")
    check("Ctrl+D doubles a line (%s), Ctrl+Z takes back one step (%s) "
          "and then the next ('%s')" % (two, one, undone),
          two == ["console.log(twice(10));"] * 2 and
          one == ["console.log(twice(10));", ""] and
          undone == "console.log(twice(21));")

    q.sendkey("ctrl-s")
    said = poll(lambda: status(q), lambda s: "Saved main.js" in s)
    check("Ctrl+S saves ('%s')" % said, "Saved main.js" in said)

    # Run
    click(q, W0 - RUN_R + BTN // 2, BAR_H // 2)
    out = poll(lambda: bottom_rows(q, 5),
               lambda r: any("exit code" in x for x in r), timeout=20)
    check("Run runs it: what it printed, and exit code 0 (%s)" % out,
          out[:4] == ["js main.js", "Hello, ThreadOS!", "42",
                      "Process finished with exit code 0"])

    # An error, and Problems
    q.sendkey("ctrl-end")
    q.sendkey("ret")
    q.type_line("missing();", enter=False)
    q.sendkey("ctrl-r")
    out = poll(lambda: bottom_rows(q, 7),
               lambda r: any("exit code" in x for x in r) and
               not any(x == "42" for x in r), timeout=20)
    click(q, *stripe_bottom(2))
    top = BOT_Y + HEAD_H
    msg = poll(lambda: ui_text(q, ED_X + 32, top, TREE_ROW_H, 300),
               lambda s: "missing" in s)
    check("an error names a line: Problems lists it ('%s'; %s)" % (msg, out),
          "missing" in msg and
          any(x.startswith("Process finished with exit code") and
              not x.endswith(" 0") for x in out))
    click(q, ED_X + 60, top + TREE_ROW_H // 2)
    at = poll(lambda: status_right(q), lambda s: s.startswith("12:"))
    check("a click on the problem takes the caret to its line ('%s')" % at,
          at.startswith("12:"))
    # A run configuration of steps: the second only if the first went well
    def configure(cmd):
        click(q, W0 - RUN_R - 8 - CFG_W // 2, BAR_H // 2)
        time.sleep(0.5)
        for _ in range(40):
            q.sendkey("backspace")
        q.type_line(cmd, enter=True)
        time.sleep(0.5)

    def finished(r):
        return any(x.startswith("Process finished") for x in r)
    configure("js nothere.js && js main.js")
    q.sendkey("ctrl-r")
    out = poll(lambda: bottom_rows(q, 7),
               lambda r: finished(r) and "js nothere.js" in r, timeout=20)
    check("a configuration of steps stops at one that fails (%s)" % out,
          "js nothere.js" in out and "js main.js" not in out and
          finished(out) and not any(x.endswith("exit code 0") for x in out))
    configure("js -e 1 && js main.js")
    q.sendkey("ctrl-r")
    out = poll(lambda: bottom_rows(q, 7),
               lambda r: finished(r) and "js main.js" in r, timeout=20)
    check("and goes on after one that went well (%s)" % out,
          "js main.js" in out and "Hello, ThreadOS!" in out and finished(out))
    configure("js main.js")

    disk = disk_file(q, "/home/admin/Projects/hello/main.js", "missing();")
    check("what was run was saved first: the disk has every line",
          "function twice(n) {" in disk and "console.log(twice(21));" in disk
          and "missing();" in disk)
    q.mouse_goto(CX + W0 // 2, CY + 10)
    q.mouse_click()                            # the window again
    away(q)

    # New File from the Project window
    click(q, STRIPE_W + SIDE_W - PLUS_R + 11, BAR_H + HEAD_H // 2)
    time.sleep(0.5)
    q.type_line("util.js", enter=True)
    files = poll(lambda: tree(q), lambda t: "util.js" in t)
    tab = poll(lambda: status(q), lambda s: "util.js" in s)
    check("the + makes a file, in the tree (%s) and open in a tab ('%s')"
          % (files, tab),
          files == ["main.js", "README.md", "util.js"] and "util.js" in tab)
    q.type_line("function half(n) {", enter=True)
    q.type_line("return n / 2;", enter=False)
    q.sendkey("ctrl-s")

    # Go to File
    q.sendkey("ctrl-o")
    time.sleep(0.5)
    q.type_line("mai", enter=False)
    px, rows_y = (W0 - POP_W) // 2, POP_Y + POP_TITLE_H + POP_IN_H
    shot = q.screen_raw()
    first = ui_text(q, px + 40, rows_y, POP_ROW_H, 200, screen=shot)
    second = ui_text(q, px + 40, rows_y + POP_ROW_H, POP_ROW_H, 200,
                     screen=shot)
    check("Go to File finds a file by part of its name ('%s', then '%s')"
          % (first, second), first == "main.js" and second == "")
    q.sendkey("ret")
    got = poll(lambda: code_row(q, 0), lambda r: r == "// hello")
    check("and Enter opens it ('%s')" % got, got == "// hello")

    # Structure
    click(q, *stripe_top(1))
    shot = q.screen_raw()
    syms = [ui_text(q, STRIPE_W + 34, BAR_H + HEAD_H + r * TREE_ROW_H,
                    TREE_ROW_H, 120, screen=shot) for r in range(3)]
    check("Structure lists the file's functions (%s)" % syms,
          syms[:2] == ["greet", "twice"])
    click(q, STRIPE_W + 50, BAR_H + HEAD_H + TREE_ROW_H + TREE_ROW_H // 2)
    at = poll(lambda: status_right(q), lambda s: s.startswith("8:"))
    check("a click on one takes the caret to it ('%s')" % at,
          at.startswith("8:"))
    click(q, *stripe_top(0))                   # Project again

    # Find: as it is typed, from the caret; Enter for the next
    q.sendkey("ctrl-f")
    time.sleep(0.3)
    q.type_line("twice", enter=False)
    fy = BAR_H + TAB_H
    count = poll(lambda: ui_text(q, ED_X + 280, fy, FIND_H, 80, fg=DIM),
                 lambda s: s == "1/2")
    q.sendkey("ret")
    after = poll(lambda: ui_text(q, ED_X + 280, fy, FIND_H, 80, fg=DIM),
                 lambda s: s == "2/2")
    at = poll(lambda: status_right(q), lambda s: s.startswith("11:"))
    check("Find counts as it is typed ('%s') and Enter goes to the next "
          "('%s', '%s')" % (count, after, at),
          count == "1/2" and after == "2/2" and at.startswith("11:"))
    q.sendkey("esc")

    # Ctrl+Y deletes the line, and Ctrl+Z brings it back
    line = "console.log(twice(21));"
    q.sendkey("ctrl-y")
    gone = poll(lambda: code_rows(q, 0, 12), lambda r: line not in r)
    q.sendkey("ctrl-z")
    back = poll(lambda: code_rows(q, 0, 12), lambda r: line in r)
    check("Ctrl+Y deletes a line and Ctrl+Z brings it back",
          line not in gone and "missing();" in gone and line in back and
          back.index(line) + 1 == back.index("missing();"))

    # The Terminal, in the project's folder
    click(q, *stripe_bottom(1))
    time.sleep(1)
    click(q, ED_X + 100, H0 - STATUS_H - ROW_H // 2)
    q.type_line("uls64")

    def listed(rows):
        text = "\n".join(rows)
        return ".code" in text and "util.js" in text
    rows = poll(lambda: bottom_rows(q, 7), listed, timeout=15)
    check("the Terminal runs a shell in the project's folder (%s)" % rows,
          listed(rows))
    q.sendkey("ctrl-o")
    time.sleep(0.5)
    q.type_line("out", enter=False)
    first = ui_text(q, (W0 - POP_W) // 2 + 40, POP_Y + POP_TITLE_H + POP_IN_H,
                    POP_ROW_H, 200)
    check("what Code keeps in .code is not offered as a file ('%s')" % first,
          first == "")
    q.sendkey("esc")
    files = poll(lambda: tree(q),
                 lambda t: t == ["main.js", "README.md", "util.js"])
    check("nor shown in the tree (%s)" % files,
          files == ["main.js", "README.md", "util.js"])

    # Closed and started again: back where it was
    x, y = light_xy(CX - 1, CY - CONTENT_DY, 0)
    q.click(x, y)
    # the dock narrows by the window's tile, and Code's tile moves with it
    q.n_win = poll(lambda: q.dock_windows(), lambda n: n == 0)
    q.launch_dock("gcode")
    check("Code starts again", find_code(q))
    away(q)
    name = poll(lambda: ui_text(q, 42, 0, BAR_H, 200, face="bold"),
                lambda s: s == "hello")
    row = poll(lambda: code_row(q, 0), lambda r: r == "// hello")
    check("and comes back to the project ('%s') with main.js open ('%s')"
          % (name, row), name == "hello" and row == "// hello")

    # Close Project: the welcome lists it
    click(q, 50, BAR_H // 2)
    time.sleep(0.5)
    click(q, 40, BAR_H - 2 + 4 + 2 * POP_ROW_H + POP_ROW_H // 2)
    listed = poll(lambda: ui_text(q, WEL_SIDE_W + 16 + 46, WEL_HEAD_H + 8, 20,
                                  200, face="bold"),
                  lambda s: s == "hello")
    check("Close Project: the welcome lists it ('%s')" % listed,
          listed == "hello")
    click(q, WEL_SIDE_W + 100, WEL_HEAD_H + WEL_ROW_H // 2)
    name = poll(lambda: ui_text(q, 42, 0, BAR_H, 200, face="bold"),
                lambda s: s == "hello")
    check("and a click opens it again ('%s')" % name, name == "hello")

    # A C++ project, from the project's menu
    click(q, 50, BAR_H // 2)
    time.sleep(0.5)
    click(q, 40, BAR_H - 2 + 4 + POP_ROW_H // 2)       # New Project...
    time.sleep(0.5)
    q.type_line("cpp-demo", enter=False)
    click(q, 60, TPL_TOP + 3 * TPL_ROW_H + TPL_ROW_H // 2)
    time.sleep(0.5)
    q.sendkey("ret")
    name = poll(lambda: ui_text(q, 42, 0, BAR_H, 200, face="bold"),
                lambda s: s == "cpp-demo")
    files = poll(lambda: tree(q),
                 lambda t: t == ["src", "greet.hpp", "main.cpp", "README.md"])
    check("the C++ template: '%s', with its sources in src (%s)"
          % (name, files),
          name == "cpp-demo" and
          files == ["src", "greet.hpp", "main.cpp", "README.md"])
    rows = poll(lambda: code_rows(q, 0, 6),
                lambda r: r[0] == "#include <iostream>")
    shot = q.screen_raw()
    directive = ink_of(q, 0, 0, 8, shot)        # #include
    header = ink_of(q, 0, 9, 19, shot)          # <iostream>
    std = ink_of(q, 5, 4, 7, shot)              # std
    check("main.cpp in C++ colours: a directive, the header it names, "
          "std (%s %s %s; %s)" % (directive, header, std, rows[5]),
          rows[0] == "#include <iostream>" and
          rows[5].startswith("    std::cout << greet(") and
          same(directive, DIRECTIVE) and same(header, STRING) and
          same(std, KEYWORD))

    # Rust and Java, in files of their own
    def new_file(name, line):
        click(q, STRIPE_W + SIDE_W - PLUS_R + 11, BAR_H + HEAD_H // 2)
        time.sleep(0.5)
        q.type_line(name, enter=True)
        time.sleep(1)
        q.type_line(line, enter=False)
        return poll(lambda: code_row(q, 0), lambda r: r == line)
    got = new_file("lib.rs", "fn add(a: i32) -> i32 {")
    shot = q.screen_raw()
    fn, add, i32 = (ink_of(q, 0, 0, 2, shot), ink_of(q, 0, 3, 6, shot),
                    ink_of(q, 0, 10, 13, shot))
    check("Rust: fn and i32 keywords, the function's name its colour "
          "('%s': %s %s %s)" % (got, fn, add, i32),
          got == "fn add(a: i32) -> i32 {" and same(fn, KEYWORD) and
          same(i32, KEYWORD) and same(add, FUNCTION))
    got = new_file("Main.java", "public class Main {")
    shot = q.screen_raw()
    pub, cls, main = (ink_of(q, 0, 0, 6, shot), ink_of(q, 0, 7, 12, shot),
                      ink_of(q, 0, 13, 17, shot))
    check("Java: public and class keywords, the name plain ('%s': %s %s %s)"
          % (got, pub, cls, main),
          got == "public class Main {" and same(pub, KEYWORD) and
          same(cls, KEYWORD) and
          not any(same(main, c) for c in (KEYWORD, STRING, COMMENT)))


def test_boot():
    """The desktop the machine starts with"""
    disk = tempfile.mktemp(suffix=".img")
    slog = tempfile.mktemp(suffix=".log")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    q = Qemu(KERNEL, INITRD, disk=disk, serial=slog)
    try:
        check("the desktop starts", q.wait_desktop())
        q.mouse_home()
        test_ide(q)
    finally:
        q.close()
        for path in (disk, slog):
            if os.path.exists(path):
                os.unlink(path)


def test_shell():
    """The desktop started from the kernel's shell, on the screen the
    loader left - the way it was started before it was the default"""
    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("SKIPPED (64): no 64-bit image")
        return
    if shutil.which("grub-mkrescue") is None or \
            shutil.which("qemu-system-x86_64") is None:
        print("SKIPPED (64): no GRUB rescue tools or QEMU")
        return

    work = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    disk = tempfile.mktemp(suffix=".img")
    slog = tempfile.mktemp(suffix=".log")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    try:
        os.makedirs(os.path.join(work, "boot", "grub"))
        shutil.copy(KERNEL64, os.path.join(work, "boot", "threados64.bin"))
        shutil.copy(INITRD64, os.path.join(work, "boot", "initrd64.img"))
        with open(os.path.join(work, "boot", "grub", "grub.cfg"), "w") as fh:
            fh.write("insmod all_video\nset gfxpayload=keep\n"
                     "set timeout=0\nset default=0\n"
                     'menuentry "ThreadOS" {\n'
                     "    multiboot2 /boot/threados64.bin\n"
                     "    module2 /boot/initrd64.img initrd64\n}\n")
        made = subprocess.run(
            ["grub-mkrescue", "-o", iso, work],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
        check("the 64-bit test disc builds", made)
        if not made:
            return

        q = Qemu(KERNEL, INITRD, disk=disk, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=60)
            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=30)
            check("the 64-bit desktop starts", q.wait_desktop())
            q.mouse_home()
            open_code(q)
        finally:
            q.close()
    finally:
        shutil.rmtree(work, ignore_errors=True)
        for path in (iso, disk, slog):
            if os.path.exists(path):
                os.unlink(path)


def main():
    print("--- Code, the development environment ---")
    test_boot()
    test_shell()
    return summary()


if __name__ == "__main__":
    sys.exit(main())
