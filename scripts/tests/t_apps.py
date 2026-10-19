#!/usr/bin/env python3
"""ThreadOS smoke test: the calculator, the calendar and paint.

Three programs that came with the new look, each held to something that
does not come from the system:

  the calculator to arithmetic: clicked and typed, 0.1 + 0.2 is 0.3
  (its numbers are millionths, not binary fractions), a third has six
  decimals, the work in progress shows above the number, and a division
  by zero says Error instead of something wrong

  the calendar to Python's own calendar: the month the machine's clock
  is in, today on the accent in the column of its weekday, the arrows
  going to the next month, and a picked day named

  paint to its own files: a stroke in a colour lands on the canvas, a BMP
  opens in the picture viewer, and the drawing can be applied as wallpaper

Run on its own or through scripts/run_tests.py.
"""

import datetime
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary
from ptest import _kernel_define as define

DARK = theme("dark")


def frame(src):
    """Where a program's window opens - slot 2 of the cascade - and its
    content's corner"""
    w, h = define(src, "W0"), define(src, "H0")
    x, y = cascade(2, w + 2, h + TITLE_H + 2)
    return w, h, x + 1, y + CONTENT_DY


def close(q, cx, cy, cw):
    q.click(cx + cw // 2, cy - TITLE_H // 2 - 1)   # its title bar: the focus
    time.sleep(0.5)
    q.sendkey("alt-f4")
    time.sleep(2)


def keys(q, text):
    for ch in text:
        q.sendkey(Qemu.KEYNAMES.get(ch, ch))
        time.sleep(0.12)
    time.sleep(1)


# --- the calculator ---------------------------------------------------------

C = "user/gcalc.c"


def calc_display(q, cx, cy, cw):
    """The number on the display, set against its right edge - in the
    text's colour, or the theme's red when it says Error"""
    best = ""
    for fg in (DARK["app_text"], DARK["app_danger"]):
        got = q.find_ui(cx + 20, cx + cw - 10, cy + define(C, "DISP_BASE"),
                        px=define(C, "DISP_PX"), fg=fg)
        if len(got) > len(best):
            best = got
    return best


def calc_expr(q, cx, cy, cw):
    return q.find_ui(cx + 20, cx + cw - 10, cy + define(C, "EXPR_BASE"),
                     px=14, fg=DARK["app_dim"])


def calc_key(q, cx, cy, row, col):
    x = define(C, "KEY_X0") + col * (define(C, "KEY_W") + define(C, "KEY_GAP"))
    y = define(C, "KEY_Y0") + row * (define(C, "KEY_H") + define(C, "KEY_GAP"))
    q.click(cx + x + define(C, "KEY_W") // 2, cy + y + define(C, "KEY_H") // 2)
    time.sleep(0.3)


def calculator(q):
    cw, ch, cx, cy = frame(C)
    q.type_line("run gcalc")
    time.sleep(3)
    q.mouse_goto(620, 300)
    time.sleep(1)
    check("the calculator opened, at nought ('%s')" % calc_display(q, cx, cy, cw),
          calc_display(q, cx, cy, cw) == "0")

    # Clicked: 7 + 8 = (the keys: row 1 col 0, row 3 col 3, row 1 col 1,
    # row 4 col 3)
    calc_key(q, cx, cy, 1, 0)
    calc_key(q, cx, cy, 3, 3)
    q.mouse_goto(620, 300)
    time.sleep(0.8)
    said = calc_expr(q, cx, cy, cw)
    check("what is being worked out shows above the number ('%s')" % said,
          said == "7 +")
    calc_key(q, cx, cy, 1, 1)
    calc_key(q, cx, cy, 4, 3)
    q.mouse_goto(620, 300)
    time.sleep(0.8)
    got = calc_display(q, cx, cy, cw)
    check("clicked, 7 + 8 = 15 ('%s')" % got, got == "15")

    for text, want, what in (("c12*3\n", "36", "typed, 12 * 3 = 36"),
                             ("c0.1+0.2\n", "0.3",
                              "0.1 + 0.2 is 0.3 - millionths, not binary "
                              "fractions"),
                             ("c1/3\n", "0.333333", "a third has six decimals"),
                             ("c2-9\n", "-7", "and a number can go below "
                              "nought"),
                             ("c5/0\n", "Error", "a division by nought says "
                              "Error")):
        keys(q, text)
        got = calc_display(q, cx, cy, cw)
        check("%s ('%s')" % (what, got), got == want)
    keys(q, "c")
    close(q, cx, cy, cw)


# --- the calendar -----------------------------------------------------------

K = "user/gcal.c"


def cal_head(q, cx, cy):
    return q.read_ui(cx + define(K, "PAD"), cy + define(K, "HEAD_BASE"), 200,
                     px=17, face="bold", fg=DARK["app_text"])


def cal_foot(q, cx, cy):
    return q.read_ui(cx + define(K, "PAD"), cy + define(K, "FOOT_BASE"), 270,
                     px=UD_PX, fg=DARK["app_text"])


def cal_cell(cx, cy, year, month, day):
    """The middle of a day's cell: the week starts on Monday"""
    slot = datetime.date(year, month, 1).weekday() + day - 1
    return (cx + define(K, "PAD") + (slot % 7) * define(K, "CELL_W") +
            define(K, "CELL_W") // 2,
            cy + define(K, "GRID_Y") + (slot // 7) * define(K, "CELL_H") +
            define(K, "CELL_H") // 2)


def calendar(q):
    cw, ch, cx, cy = frame(K)
    today = datetime.date.fromisoformat(RTC_DATE)
    q.type_line("run gcal")
    time.sleep(3)
    q.mouse_goto(620, 300)
    time.sleep(1)
    want = today.strftime("%B %Y")
    got = cal_head(q, cx, cy)
    check("the calendar opens on the month the clock is in ('%s')" % got,
          got == want)
    w, h, at = q.screen_raw()
    x, y = cal_cell(cx, cy, today.year, today.month, today.day)
    check("today is on the accent, in its weekday's column (%s)"
          % (at(x - 11, y),), near(at(x - 11, y), DARK["accent"], 6))
    x2, y2 = cal_cell(cx, cy, today.year, today.month, today.day + 1)
    check("and tomorrow is not", not near(at(x2 - 11, y2), DARK["accent"], 60))

    # The arrow on the right: the next month
    a = define(K, "ARROW")
    q.click(cx + cw - define(K, "PAD") - a // 2, cy + define(K, "ARROW_Y") + a // 2)
    q.mouse_goto(620, 300)
    time.sleep(1)
    nxt = (today.replace(day=1) + datetime.timedelta(days=32)).replace(day=1)
    got = cal_head(q, cx, cy)
    check("the arrow goes to the next month ('%s')" % got,
          got == nxt.strftime("%B %Y"))
    day = nxt.replace(day=4)
    q.click(*cal_cell(cx, cy, day.year, day.month, day.day))
    q.mouse_goto(620, 300)
    time.sleep(1)
    got = cal_foot(q, cx, cy)
    want = "%s, %d %s" % (day.strftime("%A"), day.day, day.strftime("%B %Y"))
    check("a click on a day names it ('%s')" % got, got == want)
    keys(q, "")
    q.sendkey("home")
    time.sleep(1)
    got = cal_head(q, cx, cy)
    check("Home comes back to today's month ('%s')" % got,
          got == today.strftime("%B %Y"))
    close(q, cx, cy, cw)


# --- paint ------------------------------------------------------------------

P = "user/gpaint.c"


def paint(q):
    cw, ch, cx, cy = frame(P)
    tool_h = define(P, "TOOL_H")
    q.type_line("run gpaint")
    time.sleep(3)
    # The red: the second swatch
    d, x0, step = define(P, "SW_D"), define(P, "SW_X0"), define(P, "SW_STEP")
    q.click(cx + x0 + step + d // 2, cy + (tool_h - d) // 2 + d // 2)
    time.sleep(0.5)
    # A stroke across the canvas
    sx, sy = cx + 60, cy + tool_h + 80
    q.mouse_goto(sx, sy)
    time.sleep(0.3)
    q.mouse_button(True)
    for k in range(1, 21):
        q.mouse_goto(sx + k * 12, sy)
        time.sleep(0.05)
    q.mouse_button(False)
    q.mouse_goto(620, 300)
    time.sleep(1)
    w, h, at = q.screen_raw()
    red = sum(1 for x in range(sx, sx + 240) if near(at(x, sy), (255, 59, 48), 20))
    check("a stroke in red lands on the canvas (%d of 240 pixels along it)"
          % red, red > 200)
    paper = at(cx + 30, cy + tool_h + 200)
    check("and the rest of the canvas is paper %s" % (paper,),
          paper == (255, 255, 255))

    # Save, and the viewer opens what was written
    sx2 = define(P, "SAVE_X") + define(P, "SAVE_W") // 2
    q.click(cx + sx2, cy + define(P, "BTN_Y") + define(P, "BTN_H") // 2)
    # The card slides in; with the whole suite running, writing the file
    # and the slide may take longer than a moment - so it is looked for
    # until it has settled, not at one guessed instant
    nx = SCREEN_W - NOTE_RIGHT - NOTE_W
    for _ in range(12):
        time.sleep(0.5)
        scr = q.screen_raw()
        said = (q.read_ui(nx + NOTE_TEXT_X, NOTE_TOP + NOTE_TITLE_Y, 200,
                          px=NOTE_PX, face="bold", fg=DARK["menu_text"],
                          screen=scr),
                q.read_ui(nx + NOTE_TEXT_X, NOTE_TOP + NOTE_BODY_Y, 220,
                          px=NOTE_PX, fg=DARK["menu_text"], screen=scr))
        if said[0] == "Paint" and "drawing.bmp" in said[1]:
            break
    check("Save says so in a notification (%s)" % (said,),
          said[0] == "Paint" and "drawing.bmp" in said[1])

    wall_x = define(P, "WALL_X") + define(P, "WALL_W") // 2
    q.click(cx + wall_x, cy + define(P, "BTN_Y") + define(P, "BTN_H") // 2)
    q.type_line("thread")
    wall_note = ("", "")
    for _ in range(12):
        time.sleep(0.5)
        wall_note = card(q, 0)
        if wall_note[0] == "Paint" and "wallpaper" in wall_note[1]:
            break
    check("Paint can apply the drawing as the desktop wallpaper (%s)"
          % (wall_note,),
          wall_note[0] == "Paint" and "now the desktop wallpaper" in wall_note[1])

    close(q, cx, cy, cw)
    time.sleep(1.5)
    sw, sh, at = q.screen_raw()
    wx, wy = sw * 50 // 160, sh * 32 // 120
    pixel = at(wx, wy)
    check("the exported wallpaper contains the painted red stroke (%s)"
          % (pixel,),
          pixel[0] > 200 and pixel[1] < 120 and pixel[2] < 120)

    q.click_desktop()
    time.sleep(0.5)
    q.type_line("run gimg drawing.bmp")
    time.sleep(5)
    q.mouse_goto(620, 300)
    time.sleep(1)
    vw, vh = define("user/gimg.c", "W0"), define("user/gimg.c", "H0")
    vx, vy = cascade(2, vw + 2, vh + TITLE_H + 2)
    said = q.gimg_status(vx + 1, vy + CONTENT_DY, vw, vh)
    check("the saved drawing opens in the viewer at the canvas's size ('%s')"
          % said, "%dx%d" % (cw, ch - tool_h) in said)


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
            calculator(q)
            q.click_desktop()
            time.sleep(0.5)
            calendar(q)
            q.click_desktop()
            time.sleep(0.5)
            paint(q)
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


sys.exit(main())
