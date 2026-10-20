#!/usr/bin/env python3
"""ThreadOS smoke test: the settings window.

Sections down the left, the open one on the right, each setting shown
as what it is. The window keeps none of the values - it asks the kernel
and writes to it - so every change made here is checked from outside,
through the kernel's own shell ('set <name>' prints what the kernel
holds), and the figures it shows are held to independent sources:

  the network to QEMU's user network, whose addresses are fixed
  (10.0.2.15, the gateway 10.0.2.2, the name server 10.0.2.3)

  the memory to the shell's 'mem', a different path out of the kernel

Changing anything needs admin. The first change asks for the password
in a panel over the window; a wrong one changes nothing and says so,
and the right is given back after every change, so the next one asks
again. The theme goes last: once it is light, the window is drawn in
the other colours, which is the check that it took.

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

WX, WY = cascade(2, GSET_W + 2, GSET_H + TITLE_H + 2)
CX, CY = WX + 1, WY + CONTENT_DY
AWAY = (600, 440)


def section(q, name):
    x, y = gset_section(name)
    q.click(CX + x, CY + y)
    q.mouse_goto(*AWAY)
    time.sleep(1.5)
    return q.gset_heading(CX, CY)


def typed(q, text):
    for ch in text:
        q.sendkey(Qemu.KEYNAMES.get(ch, ch))
        time.sleep(0.12)


def password(q, word):
    """Answers the panel - after checking it is there"""
    asked = q.gset_asks(CX, CY)
    typed(q, word)
    q.sendkey("ret")
    time.sleep(2)
    return asked


def status(q, want, tries=10):
    said = ""
    for _ in range(tries):
        said = q.gset_status(CX, CY)
        if said == want:
            break
        time.sleep(0.5)
    return said


def kernel_says(q, name):
    """The value the kernel holds, from its shell"""
    q.click_desktop()
    time.sleep(0.5)
    out = q.command("set " + name, "%s = " % name, timeout=15)
    m = re.search(r"%s = (\S*)" % re.escape(name), out)
    return m.group(1) if m else None


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
            q.launch_dock(6)                # the "Settings" tile
            time.sleep(3)
            q.mouse_goto(*AWAY)
            time.sleep(1.5)

            got = q.window_title(WX, WY, GSET_W + 2)
            check("the Settings tile opens the window ('%s')" % got,
                  got == "Settings")
            shot = q.screen_raw()
            sides = [q.gset_side(CX, CY, i, chosen=(i == 0), screen=shot)
                     for i in range(len(GSET_SECTIONS))]
            check("the sections are listed down the left (%s)"
                  % ", ".join(sides), sides == GSET_SECTIONS)
            head = q.gset_heading(CX, CY, screen=shot)
            name = q.gset_field_text(CX, CY, 0, screen=shot)
            check("it opens on General, with the machine's name in its "
                  "field (%s: '%s')" % (head, name),
                  head == "General" and name == "thread")

            # --- the network, against QEMU's own addresses ---------------
            head = section(q, "Network")
            want = ["10.0.2.15", "255.255.255.0", "10.0.2.2", "10.0.2.3"]
            got = []
            for _ in range(20):            # until DHCP has answered
                shot = q.screen_raw()
                got = [q.gset_info(CX, CY, i, screen=shot) for i in range(4)]
                if got == want:
                    break
                time.sleep(1)
            check("Network shows the address, mask, gateway and name server "
                  "QEMU gave (%s)" % ", ".join(got),
                  head == "Network" and got == want)

            # The arrows walk the sections when no field has the keys
            q.sendkey("down")
            time.sleep(1.5)
            head = q.gset_heading(CX, CY)
            check("the down arrow opens the next section ('%s')" % head,
                  head == "Files")

            # --- a box to tick: hidden files ------------------------------
            box = (CX + GSET_PANE_X + 8, CY + gset_ctrl_y(0) + GSET_CHIP_H // 2)
            # (a corner of the box the tick does not cross)
            fill = (box[0] + 4, box[1] + 4)
            q.click(*box)
            q.mouse_goto(*AWAY)
            time.sleep(1.5)
            asked = password(q, "thread")
            check("a change asks for the admin's password ('%s')" % asked,
                  asked == "Changing settings needs admin")
            said = status(q, "Saved")
            w, h, at = q.screen_raw()
            check("the right one saves it ('%s'), and the box is ticked"
                  % said, said == "Saved" and
                  near(at(*fill), theme("dark")["accent"], 8))
            got = kernel_says(q, "hidden")
            check("the kernel holds hidden = 1 (%s)" % got, got == "1")

            # --- a field: the machine's name ------------------------------
            section(q, "General")
            fx, fy = gset_field(0)[0]
            q.click(CX + fx, CY + fy)
            q.mouse_goto(*AWAY)
            time.sleep(0.5)
            for _ in range(len("thread")):
                q.sendkey("backspace")
                time.sleep(0.1)
            typed(q, "box")
            q.sendkey("ret")
            time.sleep(1.5)
            asked = password(q, "thread")
            check("the right was given back: the next change asks again "
                  "('%s')" % asked, asked == "Changing settings needs admin")
            said = status(q, "Saved")
            name = q.gset_field_text(CX, CY, 0)
            check("the name typed into the field is saved ('%s', '%s')"
                  % (said, name), said == "Saved" and name == "box")
            got = kernel_says(q, "name")
            check("the kernel holds name = box (%s)" % got, got == "box")

            # --- About, against the shell's 'mem' -------------------------
            q.click_desktop()
            out = q.command("mem", "RAM detected", timeout=15)
            m = re.search(r"RAM detected: (\d+) KiB", out)
            head = section(q, "About")
            shot = q.screen_raw()
            about = [q.gset_info(CX, CY, i, screen=shot) for i in range(6)]
            check("About names the system and its kernel (%s)"
                  % ", ".join(about[:2]),
                  head == "About" and about[0] == "ThreadOS" and
                  about[1].startswith("32-bit"))
            check("its memory is what the shell's 'mem' says (%s, %s KiB)"
                  % (about[2], m.group(1) if m else "?"),
                  m is not None and about[2] == m.group(1) + " KiB")
            check("and the session is a plain user's ('%s')" % about[5],
                  about[5] == "user")

            # --- regional date, time and number formats ------------------
            head = section(q, "Region")
            check("regional formats have their own section ('%s')" % head,
                  head == "Region")
            for name, row, col, value in (
                    ("datefmt", 0, 2, "iso"),
                    ("timefmt", 1, 1, "12h"),
                    ("numfmt", 2, 1, "comma")):
                section(q, "Region")
                x, y = gset_chip(row, col)
                q.click(CX + x, CY + y)
                q.mouse_goto(*AWAY)
                time.sleep(1.5)
                password(q, "thread")
                got = kernel_says(q, name)
                check("the Region control saves %s = %s (%s)"
                      % (name, value, got), got == value)

            section(q, "Region")
            x, y = gset_chip(3, 1)
            q.click(CX + x, CY + y)
            q.mouse_goto(*AWAY)
            time.sleep(1.5)
            password(q, "thread")
            got = kernel_says(q, "language")
            said = status(q, "Zapisano")
            region_top = GSET_PANE_Y0 + 3 * GSET_ROW_H
            label_baseline = CY + ui_font_baseline_in(UD_PX, region_top, 18)
            shot = q.screen_raw()
            label = q.read_ui(CX + GSET_PANE_X, label_baseline, 160,
                             px=UD_PX, fg=theme("dark")["app_text"],
                             screen=shot)
            win_title = q.window_title(WX, WY)
            check("the language control saves Polish and translates settings "
                  "(%s, %s, %s, %s)" % (got, said, label, win_title),
                  got == "pl" and said == "Zapisano" and label == "Język" and
                  win_title == "Ustawienia")

            section(q, "Region")
            x, y = gset_chip(3, 0)
            q.click(CX + x, CY + y)
            q.mouse_goto(*AWAY)
            time.sleep(1.5)
            password(q, "thread")
            got = kernel_says(q, "language")
            said = status(q, "Saved")
            win_title = q.window_title(WX, WY)
            check("English can be restored (%s, %s, %s)"
                  % (got, said, win_title),
                  got == "en" and said == "Saved" and win_title == "Settings")

            # --- the theme: refused, then taken ---------------------------
            section(q, "Appearance")
            light = gset_chip(0, 1)
            q.click(CX + light[0], CY + light[1])
            q.mouse_goto(*AWAY)
            time.sleep(1.5)
            password(q, "wrongpassword")
            said = status(q, "NOT changed (wrong password)")
            check("a wrong password changes nothing and says so ('%s')"
                  % said, said == "NOT changed (wrong password)")
            got = kernel_says(q, "theme")
            check("the kernel still holds theme = dark (%s)" % got,
                  got == "dark")

            section(q, "Appearance")
            q.click(CX + light[0], CY + light[1])
            q.mouse_goto(*AWAY)
            time.sleep(1.5)
            password(q, "thread")
            lit = False
            for _ in range(20):
                w, h, at = q.screen_raw()
                if near(at(CX + GSET_W - 20, CY + 200),
                        theme("light")["win_bg"], 4):
                    lit = True
                    break
                time.sleep(0.5)
            check("with the right one the window is drawn in the light "
                  "theme", lit)
            got = kernel_says(q, "theme")
            check("and the kernel holds theme = light (%s)" % got,
                  got == "light")
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


sys.exit(main())
