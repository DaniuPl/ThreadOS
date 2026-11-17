#!/usr/bin/env python3
"""ThreadOS smoke test: the Windows style.

'layout windows' (Settings, Desktop & Dock, "Interface style") turns the
desktop round: no bar at the top, a taskbar along the bottom with Start,
the programs and the windows in its middle and the tray and a clock at
its right, and a window's minimise, maximise and close buttons at the
right end of its title bar. Read off the screen and done with the mouse:

  the bar at the top is gone, and the taskbar's clock shows the date
  the machine has

  a program's button on the taskbar starts it; the window's maximise
  button fills the screen from the very top down to the taskbar; its
  close button closes it

  Settings' Desktop & Dock page, in this style, says the taskbar
  stands at the bottom where the dock's Position row stands otherwise

  Start opens the menu standing on the taskbar, its last entry Shut
  Down

  back to 'layout mac', the bar is at the top again

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, _kernel_define

TB_H = _kernel_define("kernel/include/look.h", "TB_H")
TB_SLOT = _kernel_define("kernel/include/look.h", "TB_SLOT")
TB_LOGO_D = _kernel_define("kernel/include/look.h", "TB_LOGO_D")
TB_SHOW_W = _kernel_define("kernel/include/look.h", "TB_SHOW_W")
TB_CLOCK_W = _kernel_define("kernel/include/look.h", "TB_CLOCK_W")
WB_W = _kernel_define("kernel/include/look.h", "WB_W")
FILES_W = _kernel_define("user/gfiles.c", "W0")
FILES_H = _kernel_define("user/gfiles.c", "H0")
DARK = theme("dark")
SEC_DESK = GSET_SECTIONS.index("Desktop & Dock")


def bar_name(q):
    """The bar's bold name: with no window in front, "Desktop" """
    return q.find_ui(BAR_NAME_X, BAR_NAME_X + 100,
                     ui_font_baseline_in(BAR_PX, 0, BAR_H),
                     px=BAR_PX, face="bold")


def slot_x(W, n, k):
    """The left edge of the taskbar's k-th button: 0 is Start"""
    return (W - (n + 1) * TB_SLOT) // 2 + k * TB_SLOT


def win_title(q, wx, wy, ww):
    """A title as the Windows style writes it: from the left, clear of
    the three buttons at the right"""
    return q.find_ui(wx + 4, wx + ww - 3 * WB_W - 4, title_baseline(wy),
                     px=TITLE_PX, face="bold")


def find_files(q, W, H):
    for slot in range(8):
        x, y = cascade(slot, FILES_W + 2, FILES_H + TITLE_H + 2, w=W, h=H)
        if "Files" in win_title(q, x, y, FILES_W + 2):
            return x, y
    return None


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop())
            W, H = q.size()
            q.mouse_home()
            check("the Mac style first: the bar at the top ('%s')"
                  % bar_name(q), bar_name(q) == "Desktop")
            q.click_desktop()
            check("'layout windows'",
                  "saved" in q.sudo("set layout windows", "saved"))
            end = time.time() + 10
            while time.time() < end and "the Windows style" not in q.serial():
                time.sleep(0.3)
            q.mouse_goto(W // 2, H // 2)
            time.sleep(1.5)
            check("no bar at the top any more ('%s')" % bar_name(q),
                  bar_name(q) == "")
            date = "%s.%s.%s" % (RTC_BASE[8:10], RTC_BASE[5:7], RTC_BASE[:4])
            # From the clock's own left edge: the network glyph left of it
            # reaches down into the date's rows. The glass under it is
            # the aurora blurred, darker to the right; its bottom row,
            # under all the lettering, is the ground column by column
            shot = q.screen_raw()
            at = shot[2]
            got = q.find_ui(W - TB_SHOW_W - 6 - TB_CLOCK_W,
                            W - TB_SHOW_W, H - TB_H + 33,
                            charset="0123456789.", px=UI_PX,
                            fg=DARK["bar_text"], screen=shot,
                            under=lambda xx, yy: at(xx, H - 2))
            check("the taskbar's clock shows the date (%s, '%s')"
                  % (date, got), got == date)

            # Files, from its button
            n = DOCK_N
            q.click(slot_x(W, n, 1) + TB_SLOT // 2, H - TB_H // 2)
            q.mouse_goto(W // 2, H // 2 - 100)
            at_win = None
            end = time.time() + 15
            while time.time() < end and at_win is None:
                time.sleep(0.5)
                at_win = find_files(q, W, H)
            check("a program's button on the taskbar starts it",
                  at_win is not None)
            if at_win:
                wx, wy = at_win
                ww = FILES_W + 2
                # maximise: the second button from the right
                q.click(wx + ww - 2 * WB_W + WB_W // 2, wy + TITLE_H // 2)
                q.mouse_goto(W // 2, H // 2)
                title = ""
                end = time.time() + 10
                while time.time() < end and "Files" not in title:
                    time.sleep(0.5)
                    title = win_title(q, 0, 0, W)
                check("its maximise button at the right fills the screen "
                      "from the very top ('%s')" % title, "Files" in title)
                _, _, at = q.screen_raw()
                ax, ay = W // 2, H - TB_H - 2
                tx, ty = slot_x(W, n + 1, 0) + TB_SLOT // 2, H - TB_H // 2
                # The mark - a cog, open in its middle - in the accent:
                # a good part of its box in that colour
                r = TB_LOGO_D // 2
                inked = sum(max(abs(a - b) for a, b in
                                zip(at(x, y), DARK["accent"])) < 40
                            for y in range(ty - r, ty + r)
                            for x in range(tx - r, tx + r))
                check("down to the taskbar, which stays over it: the window "
                      "just above it, Start's mark on it (%d of its box's "
                      "%d pixels in the accent)" % (inked, 4 * r * r),
                      at(ax, ay) != aurora_px(ax, ay, W, H) and
                      inked >= r * r // 2)
                # close: the last button
                q.click(W - WB_W // 2, TITLE_H // 2)
                q.mouse_goto(W // 2, H // 2)
                end = time.time() + 10
                while time.time() < end and \
                        "process 'gfiles' has ended" not in q.serial():
                    time.sleep(0.5)
                check("its close button at the right end closes it",
                      "process 'gfiles' has ended" in q.serial())

            # Settings' page in this style: the dock's rows are gone, a
            # note says why, where its Position row stood
            q.click(slot_x(W, n, 1 + DOCK_APPS.index("gsettings")) +
                    TB_SLOT // 2, H - TB_H // 2)
            q.mouse_goto(W // 2, H // 2 - 100)
            at_set = None
            end = time.time() + 15
            while time.time() < end and at_set is None:
                time.sleep(0.5)
                for slot in range(8):
                    x, y = cascade(slot, GSET_W + 2, GSET_H + TITLE_H + 2,
                                   w=W, h=H)
                    if win_title(q, x, y, GSET_W + 2) == "Settings":
                        at_set = (x + 1, y + CONTENT_DY, x, y)
                        break
            check("Settings starts from its taskbar button", at_set is not None)
            if at_set:
                cx, cy, sx, sy = at_set
                q.click(cx + GSET_SIDE_W // 2,
                        cy + GSET_SIDE_Y0 + SEC_DESK * GSET_SIDE_H +
                        GSET_SIDE_H // 2)
                q.mouse_goto(W // 2, H - 100)
                row = cy + GSET_PANE_Y0 + GSET_ROW_H
                said = ""
                end = time.time() + 10
                while time.time() < end and not said.startswith("In the"):
                    time.sleep(0.5)
                    said = q.read_ui(cx + GSET_PANE_X,
                                     ui_font_baseline_in(11, row, 16), 300,
                                     px=11, fg=DARK["app_dim"])
                name = q.read_ui(cx + GSET_PANE_X,
                                 ui_font_baseline_in(UD_PX, row, 18), 200,
                                 px=UD_PX, fg=DARK["app_text"])
                check("on Desktop & Dock, a note where the dock's Position "
                      "stands in the Mac style ('%s'), and no Position ('%s')"
                      % (said, name),
                      # (the full stop is too faint at 11 px to read)
                      said.rstrip(".") == "In the Windows style the taskbar "
                                         "stands at the bottom" and
                      name == "")
                q.click(sx + GSET_W + 2 - WB_W // 2, sy + TITLE_H // 2)
                end = time.time() + 10
                while time.time() < end and \
                        "process 'gsettings' has ended" not in q.serial():
                    time.sleep(0.5)

            # Start
            time.sleep(1)
            q.click(slot_x(W, n, 0) + TB_SLOT // 2, H - TB_H // 2)
            time.sleep(1)
            hgt = (2 * MENU_PAD + len(MENU_ITEMS) * MENU_ROW + MENU_SEP +
                   len(SESSION_ITEMS) * SESS_ROW)
            top = H - TB_H - 8 - hgt
            mx = max(0, min(slot_x(W, n, 0), W - MENU_W))
            y = session_y("Shut Down...", top=top)
            q.mouse_goto(mx + MENU_W - 12, y)
            time.sleep(0.6)
            said = q.read_ui(mx + 10 + MENU_ICON + 10,
                             ui_font_baseline_in(MENU_PX, y - SESS_ROW // 2,
                                                 SESS_ROW),
                             width=120, px=MENU_PX, fg=DARK["accent_text"])
            check("Start opens the menu standing on the taskbar, Shut Down "
                  "last ('%s')" % said, said == "Shut Down...")
            q.sendkey("esc")
            q.mouse_goto(W // 2, H // 2)

            q.click_desktop()
            q.sudo("set layout mac", "saved")
            end = time.time() + 10
            while time.time() < end and bar_name(q) != "Desktop":
                time.sleep(0.5)
            check("'layout mac': the bar is back", bar_name(q) == "Desktop")
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


sys.exit(main())
