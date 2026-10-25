#!/usr/bin/env python3
"""ThreadOS smoke test: an admin session stays active in GUI apps.

The desktop signs in as the administrator before this group starts.
The terminal and settings app should keep that session, so ordinary
system changes do not ask for the password again. Risky account changes
are covered by the multiuser group, which checks their reauthentication.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

# The terminal opens in slot 2: content at (95,65), padded by 4, rows
# 10 apart, 400x240 of content.
WX, WY = 94, 54
CX, CY = WX + 1, WY + CONTENT_DY
PAD, ROW_H = TERM_PAD, TERM_ROW_H
COLS, ROWS = TERM_COLS, TERM_ROWS
USERS_W, USERS_H = 520, 340


def read_rows(q, ox, oy, cols, rows):
    """The terminal's text, read off the screen in the lettering it is
    drawn in: a grid of cells, each read on its own (ptest.read_cells)"""
    return q.read_cells(ox, oy, cols, rows, cell=TERM_CELL, row_h=ROW_H,
                        base=TERM_BASE, px=TERM_PX)


def wait_row(q, want, geom, tries=15):
    seen = []
    for _ in range(tries):
        seen = read_rows(q, **geom)
        if any(want in r for r in seen):
            return seen
        time.sleep(0.5)
    return seen


def window_says(q, cx, cy, want, tries=8):
    """Does the line at the bottom of the settings window say that? It
    is redrawn when the program gets round to it, so we look again
    rather than assume."""
    said = ""
    for _ in range(tries):
        said = q.gset_status(cx, cy)
        if want in said:
            return True
        time.sleep(0.6)
    print("      (the window says '%s')" % said)
    return False


def typed(q, text):
    for ch in text:
        q.sendkey(q.KEYNAMES.get(ch, ch))
        time.sleep(0.12)


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

            # --- the terminal ------------------------------------------
            q.launch_dock("gterm")                # the "Term" tile
            time.sleep(3)
            q.mouse_goto(CX + 200, CY + 150)
            q.mouse_click()
            time.sleep(1)
            q.mouse_goto(600, 380)
            time.sleep(1)

            q.type_line("whoami")
            rows = wait_row(q, "admin - you can change the system", geom)
            check("the terminal inherits the logged-in admin session",
                  "admin - you can change the system" in "\n".join(rows))

            # The admin session is already authenticated, so a normal
            # system operation should not open a second password prompt.
            q.type_line("sudo cp termout system.cfg")
            rows = wait_row(q, "sudo cp", geom)
            body = "\n".join(rows)
            check("sudo uses the existing session without prompting",
                  "password:" not in body and
                  "no permission" not in body.split("sudo cp")[-1])

            q.type_line("whoami")
            time.sleep(2)
            rows = read_rows(q, **geom)
            check("the terminal remains in the admin session",
                  "\n".join(rows).count("admin - you can change the system") >= 2)

            # --- the settings window ------------------------------------
            q.launch_dock("gsettings")                # the "Settings" tile
            time.sleep(3)
            q.mouse_goto(600, 380)
            time.sleep(1.5)

            # Slot 3 of the cascade - which on this screen is back at
            # the start. A background asked for under Appearance.
            sx, sy = cascade(3, GSET_W + 2, GSET_H + TITLE_H + 2)
            SX, SY = sx + 1, sy + CONTENT_DY
            q.click(SX + gset_section("Appearance")[0],
                    SY + gset_section("Appearance")[1])
            time.sleep(1)
            green = gset_swatch(3)
            q.click(SX + green[0], SY + green[1])
            time.sleep(2)
            q.mouse_goto(600, 380)
            time.sleep(1.5)

            asked = q.gset_asks(SX, SY)
            check("an admin setting change does not open a password panel",
                  asked == "")
            time.sleep(1.5)
            q.mouse_goto(600, 380)
            time.sleep(1.5)
            check("the setting is saved in the persistent admin session",
                  window_says(q, SX, SY, "Saved"))

            # Account management is a desktop app shipped in both initrds.
            q.launch_menu("gusers")
            time.sleep(2)
            ux, uy = cascade(4, USERS_W + 2,
                             USERS_H + TITLE_H + 2)
            check("the account manager opens from the desktop menu",
                  q.window_title(ux, uy, USERS_W + 2) == "User Accounts")
            ucx, ucy = ux + 1, uy + CONTENT_DY
            q.click(ucx + 300, ucy + 105)
            typed(q, "guiuser")
            q.click(ucx + 300, ucy + 157)
            typed(q, "guipass")
            q.click(ucx + 300, ucy + 209)
            typed(q, "guipass")
            q.click(ucx + 300, ucy + 261)
            typed(q, "thread")
            q.click(ucx + 72, ucy + USERS_H - 28)
            time.sleep(2)
            row_base = ucy + ui_font_baseline_in(UD_PX, 118, 18)
            second_account = q.read_ui(ucx + 24, row_base, 100, px=UD_PX)
            check("the 32-bit account manager creates an account through the syscall",
                  second_account == "guiuser")

            q.click(ucx + 50, ucy + 124)
            q.click(ucx + 300, ucy + 261)
            typed(q, "thread")
            q.click(ucx + 316, ucy + USERS_H - 28)
            time.sleep(1)
            role = q.read_ui(ucx + 160, row_base, 55, px=UD_PX)
            check("the account manager changes a user's role",
                  role == "admin")

            q.click(ucx + 300, ucy + 157)
            typed(q, "resetpass")
            q.click(ucx + 300, ucy + 209)
            typed(q, "resetpass")
            q.click(ucx + 300, ucy + 261)
            typed(q, "thread")
            q.click(ucx + 194, ucy + USERS_H - 28)
            time.sleep(1)
            status_base = ucy + ui_font_baseline_in(
                UD_PX, USERS_H - 70, 20)
            status = q.read_ui(ucx + 16, status_base, 220, px=UD_PX)
            check("the account manager resets a password after admin reauthentication",
                  status == "Account updated")

            q.click(ucx + 300, ucy + 261)
            typed(q, "thread")
            q.click(ucx + 438, ucy + USERS_H - 28)
            time.sleep(1)
            second_account = q.read_ui(ucx + 24, row_base, 100, px=UD_PX)
            check("the account manager removes the selected account",
                  second_account == "")
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
