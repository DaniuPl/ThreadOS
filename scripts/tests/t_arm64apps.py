#!/usr/bin/env python3
"""ThreadOS smoke test: the phone's dock, shade, settings and apps.

ThreadOS mobile (kernelarm64/mobile.c, mobapps.c) on QEMU's virt as a
phone, with a battery said through fw_cfg (opt/threados/battery, "73
discharging" - virt has none), a virtio network card, a clock at 15:34
and a package whose program posts a notification (libc/tests/notetest.c,
SYS64_NOTIFY). Everything is read off the screen - glyph by glyph
against FreeType, colours pixel by pixel - and the serial port:

  the lock screen lists the kernel's notification that the network is
  up, with the address DHCP gave; the status bar has the battery's 73%
  and the network's four bars

  the dock: Notes, Calculator, Settings and the package's application,
  each its colour

  Calculator: sums typed on its keys - whole numbers, decimals, a chain,
  a sign changed, a division by zero, a result past its digits - give
  what Python's decimal module makes of them at six places, rounded half
  up; the screen shows the last

  Notes: a capital by shift, a word, a space, Polish letters from their
  layer, a backspace that takes a whole Polish letter, all read back
  off the page - and still there after going home and back

  Settings: the dark look off (the page light) and on; the accent (a
  switch in it); a wallpaper (home's background the gradient computed
  here); round icons (a tile's corner gone); brightness (every pixel
  scaled, as computed here); battery saver (the battery yellow, the
  screen held to 60%); the percentage hidden; silent mode (the crossed
  bell); the 12-hour clock (3:34, not 15:34); airplane mode (the bars
  gone, the network's page "Off", and back); Android's three buttons
  (no pill; back, recent apps, home); the display size at 150% (the
  screen 480 x 853, the grid's tile where that scale puts it, and a tap
  at the new size's places reaching the dock)

  the shade: pulled down from the status bar; the flashlight's tile
  switches it on and off; with do not disturb on,
  the program's notification makes no banner but is first in the list,
  the network's after it; off, it makes a banner; Clear empties the list
"""

import decimal
import math
import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import ptest                 # noqa: E402
from ptest import *          # noqa: F401,F403,E402
from ptest import (check, summary, noarm64, Phone, ARM64_INITRD,  # noqa: E402
                   _kernel_define)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
MOB, APPS = "kernelarm64/mobile.c", "kernelarm64/mobapps.c"
NOTETEST = os.path.join(os.path.dirname(ARM64_INITRD), "arm64", "libc",
                        "notetest.elf")
K = 2
LW, LH = Phone.LOGICAL           # at the largest display size
WHITE, MUTED = (255, 255, 255), (0xAE, 0xB6, 0xCC)
POLISH = "ąćęłńóśźżĄĆĘŁŃÓŚŹŻ"
CHARS = ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
         ".,:%- " + POLISH)


def m(name):
    return _kernel_define(MOB, name)


def a(name):
    return _kernel_define(APPS, name)


def rgb(v):
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def dimmed(c, level):
    """video_present's arithmetic: each channel times level, over 256"""
    return tuple(x * level >> 8 for x in c)


def array_colours(src, name):
    text = open(os.path.join(ROOT, src)).read()
    body = re.search(r"%s\[\] = \{(.*?)\n\};" % name, text, re.S).group(1)
    return [int(c, 16) for c in re.findall(r'"[^"]+",\s*\w+,\s*0x([0-9A-Fa-f]+)',
                                           body)]


class App:
    def __init__(self, q):
        self.q = q

    def said(self, text, timeout=20):
        return self.q.wait_serial(text, timeout=timeout)

    def screen(self):
        time.sleep(0.6)
        return self.q.screen_raw()

    def at(self, x, y, scr=None):
        """A logical pixel's colour (its top-left panel pixel)"""
        w, h, at = scr or self.screen()
        return at(x * K, y * K)

    def read(self, x0, x1, baseline, px, face="sans", fg=WHITE, scr=None):
        return self.q.find_ui(x0 * K, x1 * K, baseline * K, px=px * K,
                              face=face, fg=fg, charset=CHARS,
                              screen=scr or self.screen())

    def home(self):
        seen = len(self.q.serial())
        self.q.touch([(180, 632), (180, 560), (180, 470)])
        self.q.wait_serial("[mobile] home", timeout=20)
        return seen

    def bars(self, scr=None):
        """The network's bars: white runs three logical pixels wide in the
        band left of the battery's per cent, at a row all four cover"""
        w, h, at = scr or self.screen()
        runs, x = [], 200 * K
        while x < 290 * K:
            if at(x, 17 * K) == WHITE:
                s = x
                while x < 290 * K and at(x, 17 * K) == WHITE:
                    x += 1
                runs.append((s, x - s))
            x += 1
        return [r for r in runs if r[1] == 3 * K]


def dock_box(lw=LW, lh=LH):
    """The dock's panel: MOB_DOCK_PCT of the width, centred"""
    w = lw * m("MOB_DOCK_PCT") // 100
    return (lw - w) // 2, lh - m("MOB_DOCK_UP"), w, m("MOB_DOCK_H")


def dock_xy(i, n, lw=LW, lh=LH):
    """mobile.c's dock_xy: the icons evenly apart, the ends' room the
    same as between them"""
    tile = m("MOB_DOCK_TILE")
    dx, dy, dw, dh = dock_box(lw, lh)
    gap = (dw - n * tile) // (n + 1)
    return dx + gap + i * (tile + gap), dy + (dh - tile) // 2


# --- Calculator -----------------------------------------------------------

CALC_STEP = (LW - 24) // 4                       # mobapps.c's CALC_STEP_X
CALC_TOP = LH - a("CALC_KEYS_UP")                # its CALC_KEY_Y


def calc_key(label):
    rows = [["C", "±", "%", "÷"], ["7", "8", "9", "×"], ["4", "5", "6", "−"],
            ["1", "2", "3", "+"], ["0", "0", ".", "="]]
    w = CALC_STEP - a("CALC_KEY_GAP")
    for r, row in enumerate(rows):
        if label in row:
            c = row.index(label)
            return (a("CALC_KEY_X") + c * CALC_STEP + w // 2,
                    CALC_TOP + r * a("CALC_STEP_Y") + a("CALC_KEY_H") // 2)
    raise KeyError(label)


def decimal_answer(x, op, y):
    """What the calculator should say: six places, half up, no zeros
    trailing, Error for a division by zero or past twelve digits"""
    D = decimal.Decimal
    ctx = decimal.Context(prec=60)
    x, y = D(x), D(y)
    if op == "÷" and y == 0:
        return "Error"
    r = {"+": x + y, "−": x - y, "×": x * y,
         "÷": ctx.divide(x, y) if y else None}[op]
    r = r.quantize(D("0.000001"), rounding=decimal.ROUND_HALF_UP)
    if abs(r) >= D(10) ** 12:
        return "Error"
    s = format(r.normalize(), "f")
    return "0" if s in ("-0", "0") else s


def calculator(app):
    q = app.q
    sums = [("12×3=", ("12", "×", "3")), ("C7÷2=", ("7", "÷", "2")),
            ("C1÷3=", ("1", "÷", "3")), ("C2÷3=", ("2", "÷", "3")),
            ("C0.1+0.2=", ("0.1", "+", "0.2")),
            ("C123456789×1000=", ("123456789", "×", "1000")),
            ("C999999×1000000=", ("999999", "×", "1000000")),
            ("C5÷0=", ("5", "÷", "0")),
            ("C2.5×4−1=", ("10", "−", "1")),
            ("C9±×3=", ("-9", "×", "3"))]
    bad = []
    for keys, (x, op, y) in sums:
        seen = len(q.serial())
        for k in keys:
            q.tap(*calc_key(k))
        q.wait_serial("[calc] ", timeout=20)
        line = q.serial()[seen:].encode("latin1").decode("utf-8", "replace")
        got = re.search(r"\[calc\] (.*?)\r?\n", line)
        want = f"{x} {op} {y} = {decimal_answer(x, op, y)}"
        if not got or got.group(1) != want:
            bad.append((want, got.group(1) if got else None))
    check(f"Calculator's sums are what Python's decimal module makes of "
          f"them (wrong: {bad or 'none'})", not bad)
    shown = app.read(0, LW, CALC_TOP - a("CALC_RESULT_UP"),
                     a("CALC_RESULT_PX"))
    check(f"and the screen shows the last result, -27 ({shown!r})",
          shown == "-27")


# --- Notes ----------------------------------------------------------------

LETTERS = ["qwertyuiop", "asdfghjkl", "zxcvbnm"]
POLISH_ROW = "ąćęłńóśźż"


PITCH = (LW - 10) // 10                          # NOTES_KEY_PITCH
KB_TOP = LH - a("NOTES_KB_UP")                    # NOTES_KB_Y


def note_key(row, i, count):
    x0 = (LW - count * PITCH + 3) // 2
    return (x0 + i * PITCH + (PITCH - a("NOTES_KEY_GAP")) // 2,
            KB_TOP + row * a("NOTES_KEY_STEP") + a("NOTES_KEY_H") // 2)


def notes(app):
    q = app.q
    kb, step, h = KB_TOP, a("NOTES_KEY_STEP"), a("NOTES_KEY_H")
    shift = (27, kb + 2 * step + h // 2)
    back = (LW - 27, kb + 2 * step + h // 2)
    space = (183, kb + 3 * step + h // 2)
    polish = (83, kb + 3 * step + h // 2)

    def letter(c):
        for r, row in enumerate(LETTERS):
            if c in row:
                q.tap(*note_key(r, row.index(c), len(row)))
                return
        raise KeyError(c)

    q.tap(*shift)
    for c in "hello":
        letter(c)
    q.tap(*space)
    for c in "world":
        letter(c)
    q.tap(*space)
    q.tap(*polish)
    for c in "żół":
        q.tap(*note_key(0, POLISH_ROW.index(c), len(POLISH_ROW)))
    q.tap(*polish)
    letter("w")
    first = app.read(a("NOTES_TEXT_X"), LW, a("NOTES_TEXT_Y"), a("NOTES_PX"))
    check(f"Notes: shift, letters, spaces and the Polish layer write "
          f"'Hello world żółw' ({first!r})", first == "Hello world żółw")
    q.tap(*back)
    q.tap(*back)
    first = app.read(a("NOTES_TEXT_X"), LW, a("NOTES_TEXT_Y"), a("NOTES_PX"))
    check(f"backspace takes a character at a time, a Polish one whole "
          f"({first!r})", first == "Hello world żó")
    app.home()
    q.tap(*[c + m("MOB_DOCK_TILE") // 2 for c in dock_xy(0, 4)])
    app.said("[mobile] Notes (dock)")
    first = app.read(a("NOTES_TEXT_X"), LW, a("NOTES_TEXT_Y"), a("NOTES_PX"))
    check(f"and the note is still there after home and back ({first!r})",
          first == "Hello world żó")


# --- Settings ---------------------------------------------------------------

def row_y(k):
    return a("SET_CARD_Y") + k * a("SET_ROW_H") + a("SET_ROW_H") // 2


# Where the per cent begins: past the network's bars, short of the battery
PCT_X = LW - m("MOB_BATT_RIGHT") - m("MOB_BATT_W") - 36

MAIN = {"airplane": 0, "network": 1, "display": 2, "look": 3, "sound": 4,
        "battery": 5, "navigation": 6, "time": 7, "about": 8}
DISPLAY = {"brightness": 0, "size": 1}
LOOK = {"matte": 0, "glass": 1, "solid": 2, "dark": 3, "wallpaper": 4,
        "accent": 5, "round": 6}
WALLPAPER = {"aurora": 0, "ocean": 1, "sunset": 2, "graphite": 3}
SWITCH_X = LW - 32 - a("SET_SWITCH_W") // 2


def back(app, to="Settings"):
    app.q.tap(40, _kernel_define(APPS, "APP_TITLE_Y") - 8)
    app.said("[settings] " + to)


def page(app, name):
    app.q.tap(180, row_y(MAIN[name]))
    time.sleep(0.4)


def wallpaper_px(index, y_panel, h_panel):
    grads = {1: (0x0B3D91, 0x00C6FF), 2: (0xFF5E62, 0xFF9966),
             3: (0x3A3A3C, 0x000000)}
    top, bot = grads[index]
    out = []
    for sh in (16, 8, 0):
        ca, cb = (top >> sh) & 0xFF, (bot >> sh) & 0xFF
        n = (cb - ca) * y_panel
        q = abs(n) // (h_panel - 1)          # C's division: toward zero
        out.append(ca + (q if n >= 0 else -q))
    return tuple(out)


def settings(app):
    q = app.q
    q.tap(*[c + m("MOB_DOCK_TILE") // 2 for c in dock_xy(2, 4)])
    app.said("[mobile] Settings (dock)")
    dark_bg, light_bg = rgb(0x101218), rgb(0xF2F2F7)
    check("Settings opens on its list, dark",
          app.at(5, 300) == dark_bg)

    # Appearance: the look, the accent, the wallpaper, the icons
    page(app, "look")
    app.said("[settings] Appearance")
    q.tap(SWITCH_X, row_y(LOOK["dark"]))
    app.said("[settings] Dark mode off")
    light = app.at(5, 600)
    q.tap(SWITCH_X, row_y(LOOK["dark"]))
    app.said("[settings] Dark mode on")
    check(f"dark mode off makes the page light ({light}), on dark again",
          light == light_bg and app.at(5, 600) == dark_bg)
    green = array_accent(2)
    q.tap(a("SET_DOT_X") + 2 * a("SET_DOT_STEP"), row_y(LOOK["accent"]))
    app.said("[settings] accent Green")
    switch = app.at(LW - 32 - a("SET_SWITCH_W") + 8, row_y(LOOK["dark"]))
    check(f"the accent chosen is the switches' colour ({switch})",
          switch == rgb(green))
    q.tap(180, row_y(LOOK["wallpaper"]))
    app.said("[settings] Wallpaper")
    q.tap(180, row_y(WALLPAPER["sunset"]))
    app.said("[settings] wallpaper Sunset")
    back(app, "Appearance")
    q.tap(SWITCH_X, row_y(LOOK["round"]))
    app.said("[settings] Round icons on")
    back(app)
    page(app, "display")
    app.said("[settings] Display")
    # Brightness: half; every pixel of the screen halved
    sx, sw = a("SET_SLIDER_X"), a("SET_SLIDER_W")
    q.tap(sx + sw // 2, row_y(DISPLAY["brightness"]))
    app.said("[mobile] brightness 50%")
    level = 50 * 256 // 100
    half = app.at(5, 600)
    q.tap(sx + sw + 6, row_y(DISPLAY["brightness"]))
    app.said("[mobile] brightness 100%")
    check(f"brightness at half scales the screen's pixels ({half}, "
          f"{dimmed(dark_bg, level)} worked out)",
          half == dimmed(dark_bg, level))

    seen = app.home()
    w, h, at = scr = app.screen()
    y = 45 * K
    check(f"home's wallpaper is the sunset's slope ({at(2, y)} at row {y}, "
          f"{wallpaper_px(2, y, h)} worked out)",
          at(2, y) == wallpaper_px(2, y, h))
    gx, gy = m("MOB_GRID_X"), m("MOB_GRID_Y")
    corner = at((gx + 5) * K, (gy + 5) * K)
    middle = at((gx + 4) * K, (gy + m("MOB_TILE") // 2) * K)
    check(f"with round icons a tile's corner is the wallpaper's "
          f"({corner}), its middle its colour ({middle})",
          corner != middle and corner == at(2, (gy + 5) * K))

    # Battery saver, the percentage, silent mode, the 12-hour clock
    q.tap(*[c + m("MOB_DOCK_TILE") // 2 for c in dock_xy(2, 4)])
    app.said("[mobile] Settings (dock)")
    page(app, "battery")
    pct = app.read(PCT_X, LW, 19, m("MOB_STATUS_PX"))
    check(f"the status bar has the battery fw_cfg gave ({pct!r})",
          pct == "73%")
    q.tap(SWITCH_X, row_y(2))
    app.said("[settings] Battery saver on")
    bx = LW - m("MOB_BATT_RIGHT") - m("MOB_BATT_W")
    fill = app.at(bx + 3, 14)
    want = dimmed(rgb(0xFFD60A), 60 * 256 // 100)
    check(f"battery saver: the battery yellow and the screen at 60% "
          f"({fill}, {want} worked out)", fill == want)
    q.tap(SWITCH_X, row_y(2))
    app.said("[settings] Battery saver off")
    q.tap(SWITCH_X, row_y(3))
    app.said("[settings] Percentage in the bar off")
    pct = app.read(PCT_X, LW, 19, m("MOB_STATUS_PX"))
    q.tap(SWITCH_X, row_y(3))
    app.said("[settings] Percentage in the bar on")
    pct_on = app.read(PCT_X, LW, 19, m("MOB_STATUS_PX"))
    check(f"the battery's per cent hidden ({pct!r}) and shown "
          f"({pct_on!r}) by Settings", "%" not in pct and pct_on == "73%")
    back(app)
    page(app, "sound")
    q.tap(SWITCH_X, row_y(1))
    app.said("[settings] Silent mode on")
    w, h, at = app.screen()
    red = sum(1 for x in range(150 * K, 300 * K) for y in range(0, 26 * K)
              if at(x, y) == (0xFF, 0x45, 0x3A))
    q.tap(SWITCH_X, row_y(1))
    app.said("[settings] Silent mode off")
    w, h, at = app.screen()
    red_off = sum(1 for x in range(150 * K, 300 * K)
                  for y in range(0, 26 * K) if at(x, y) == (0xFF, 0x45, 0x3A))
    check(f"silent mode crosses a bell out in the status bar ({red} red "
          f"pixels, {red_off} without)", red > 10 and red_off == 0)
    back(app)
    page(app, "time")
    q.tap(SWITCH_X, row_y(0))
    app.said("[settings] 24-hour clock off")
    t12 = app.read(0, LW // 2, 19, 14, face="bold")
    q.tap(SWITCH_X, row_y(0))
    app.said("[settings] 24-hour clock on")
    t24 = app.read(0, LW // 2, 19, 14, face="bold")
    check(f"the 12-hour clock says 3:3x where the 24-hour says 15:3x "
          f"({t12!r}, {t24!r})", re.fullmatch(r"3:3\d", t12 or "") and
          re.fullmatch(r"15:3\d", t24 or ""))
    back(app)

    # Airplane mode: the card off, the bars gone
    before = app.bars()
    q.tap(SWITCH_X, row_y(MAIN["airplane"]))
    app.said("[net] airplane mode on")
    during = app.bars()
    page(app, "network")
    state = app.read(LW // 2, LW, row_y(0) + 6, a("SET_PX"), fg=MUTED)
    back(app)
    q.tap(SWITCH_X, row_y(MAIN["airplane"]))
    app.said("[net] airplane mode off")
    after = app.bars()
    check(f"airplane mode: four bars before ({len(before)}), none during "
          f"({len(during)}), the network 'Off' ({state!r}), four after "
          f"({len(after)})", len(before) == 4 and not during and
          state == "Off" and len(after) == 4)


def array_accent(i):
    text = open(os.path.join(ROOT, APPS)).read()
    body = re.search(r"accents\[MOB_ACCENTS\] = \{(.*?)\};", text, re.S)
    return [int(c, 16) for c in re.findall(r"0x([0-9A-Fa-f]+)",
                                           body.group(1))][i]


# --- the shade and notifications -------------------------------------------

def shade(app):
    q = app.q
    app.home()
    q.touch([(180, 8), (180, 150), (180, 300)])
    app.said("[mobile] shade (opened)")
    tile = m("MOB_SHADE_TILE_Y") + m("MOB_SHADE_TILE_STEP") + 20
    dnd_x = 12 + (LW - 36) // 2 + 12 + 40           # MOB_SHADE_TILE_W
    q.tap(dnd_x, tile)                               # do not disturb
    app.said("[mobile] do not disturb on")
    torch = m("MOB_SHADE_TILE_Y") + 2 * m("MOB_SHADE_TILE_STEP") + 20
    q.tap(40, torch)
    app.said("[mobile] flashlight on")
    q.tap(40, torch)
    app.said("[mobile] flashlight off")
    q.touch([(180, 500), (180, 350), (180, 200)])
    app.said("[mobile] shade (closed)")

    seen = len(q.serial())
    q.tap(*[c + m("MOB_DOCK_TILE") // 2 for c in dock_xy(3, 4)])
    q.wait_serial("[notify] Test: Hello from a program", timeout=60)
    by = m("MOB_BANNER_Y")
    quiet = app.read(26, LW, by + 26, 15, face="bold")
    check(f"with do not disturb a program's notification makes no banner "
          f"({quiet!r})", quiet != "Test")
    q.touch([(180, 8), (180, 150), (180, 300)])
    app.said("[mobile] shade (opened)")
    cy, step = m("MOB_SHADE_CARD_Y"), m("MOB_SHADE_CARD_STEP")
    scr = app.screen()
    cards = [(app.read(28, LW - 60, cy + i * step + 24, 15, face="bold",
                       scr=scr),
              app.read(28, LW - 20, cy + i * step + 44, 13, fg=MUTED,
                       scr=scr)) for i in range(2)]
    check(f"but the shade lists it first, the network's after it "
          f"({cards})", cards == [("Test", "Hello from a program"),
                                  ("Network", "Connected, 10.0.2.15")])
    q.tap(dnd_x, tile)
    app.said("[mobile] do not disturb off")
    q.touch([(180, 500), (180, 350), (180, 200)])
    app.said("[mobile] shade (closed)")
    q.tap(*[c + m("MOB_DOCK_TILE") // 2 for c in dock_xy(3, 4)])
    q.wait_serial("[notify] Test", timeout=60)
    banner = ""
    end = time.time() + 4
    while banner != "Test" and time.time() < end:
        banner = app.read(26, LW, by + 26, 15, face="bold")
    check(f"without it the notification is a banner ({banner!r})",
          banner == "Test")
    q.touch([(180, 8), (180, 150), (180, 300)])
    app.said("[mobile] shade (opened)")
    q.tap(LW - 40, m("MOB_SHADE_NOTE_Y") - 6)
    app.said("[mobile] notifications cleared")
    empty = app.read(0, LW, cy + 34, 15, fg=MUTED)
    check(f"Clear empties the list ({empty!r})", empty == "No notifications")
    q.touch([(180, 500), (180, 350), (180, 200)])
    app.said("[mobile] shade (closed)")


def navigation(app):
    q = app.q
    q.tap(*[c + m("MOB_DOCK_TILE") // 2 for c in dock_xy(2, 4)])
    app.said("[mobile] Settings (dock)")
    page(app, "navigation")
    pill_y = LH - m("MOB_PILL_UP")
    nav_y = LH - m("MOB_NAV_UP")
    pill_before = app.at(150, pill_y + 2)
    q.tap(180, row_y(1))
    app.said("[settings] Three buttons, as Android")
    w, h, at = scr = app.screen()
    pill_after = at(150 * K, (pill_y + 2) * K)
    ring = at(LW // 2 * K, (nav_y + 18 - 9) * K + 1)
    check(f"Android's buttons: the pill gone ({pill_before} -> "
          f"{pill_after}), the home button's ring there ({ring})",
          pill_before != pill_after and ring != pill_after)
    ny = (nav_y + LH) // 2
    q.tap(LW // 4, ny)
    out = app.said("[settings] Settings")
    q.tap(LW * 3 // 4, ny)
    app.said("[mobile] recent apps (the recent apps button)")
    first = app.read(80, LW, m("MOB_RECENT_Y") + 39, 17, face="bold")
    check(f"back goes up a page; recent apps lists Settings first "
          f"({first!r})", "[mobile] back" in out and first == "Settings")
    q.tap(LW // 2, ny)
    app.said("[mobile] home (the home button)")
    # back to gestures for what follows
    q.tap(*[c + m("MOB_DOCK_TILE") // 2 for c in dock_xy(2, 4)])
    app.said("[mobile] Settings (dock)")
    page(app, "navigation")
    q.tap(180, row_y(0))
    app.said("[settings] Gestures, as an iPhone")


def lift(c):
    """mobile.c's lift: GLASS_LIFT/256 of the way to white"""
    return tuple(v + ((255 - v) * m("GLASS_LIFT") >> 8) for v in c)


def matte(c):
    """The matte dock: white at 60/255 over what is under it, as
    kernel/gfx.c blends: (colour a + under (255 - a) + 127) / 255"""
    return tuple((255 * 60 + v * (255 - 60) + 127) // 255 for v in c)


DARK_SURFACE = (0x1C, 0x1C, 0x1E)           # the dark look's, which glass
                                            # is tinted towards


def frosted(col):
    """mobile.c's frost over a panel's patch whose rows are each one
    colour (the Ocean's slope, x alone does not change it), 'col' its
    rows top to bottom: across, a row stays as it is; down, the average
    of FROST_R rows each way - fewer at the patch's ends - twice"""
    R = m("FROST_R")
    for _ in range(2):
        n = len(col)
        col = [tuple(sum(c[ch] for c in col[max(0, j - R):min(n, j + R + 1)])
                     // (min(n, j + R + 1) - max(0, j - R)) for ch in range(3))
               for j in range(n)]
    return col


def tint(c, surface=DARK_SURFACE):
    """mobile.c's tint: FROST_TINT/256 of the way to the surface"""
    t = m("FROST_TINT")
    return tuple((a * (256 - t) + b * t) >> 8 for a, b in zip(c, surface))


def glass_rim(col, j):
    """mobile.c's glass_on, j panel rows below the top edge of a panel
    and away from its corners: 'col' is what lies under it, row by row,
    frosted; row j + the lens's shift of the red, green or blue - each
    its own - lifted, tinted, and lightened by the edge's bevel, which
    faces up towards the light"""
    lens = m("GLASS_LENS") * K
    depth = lens - j
    shift = m("GLASS_BEND") * depth * depth // lens
    spread = m("GLASS_SPREAD")
    face = 256 * 181 // 256
    bevel = face * m("GLASS_BEVEL") * depth * depth // (256 * lens * lens)
    out = []
    for ch in range(3):
        row = min(j + shift * (16 + (ch - 1) * spread) // 16, len(col) - 1)
        out.append(min(255, tint(lift(col[row]))[ch] + bevel))
    return tuple(out)


def dimmed_by_glass(c):
    """mobile.c's backdrop_dim: GLASS_DIM/256 of each colour"""
    return tuple(v * m("GLASS_DIM") >> 8 for v in c)


def glass(app):
    """Liquid Glass bends the wallpaper at the dock's rim: a few panel
    rows below its top edge, between two icons, shows the Ocean slope
    from further down - lens - d rows, squared over the lens - made a
    little brighter, its red, green and blue from rows apart, as glass
    parts colours, and lighter for the bevel; and the rim catches the
    light where it turns round to face it, not along an edge"""
    q = app.q
    app.home()
    q.tap(*[c + m("MOB_DOCK_TILE") // 2 for c in dock_xy(2, 4)])
    app.said("[mobile] Settings (dock)")
    page(app, "look")
    q.tap(180, row_y(LOOK["wallpaper"]))
    q.tap(180, row_y(WALLPAPER["ocean"]))
    app.said("[settings] wallpaper Ocean")
    back(app, "Appearance")
    dx, dy, dw, dh = dock_box()
    tile = m("MOB_DOCK_TILE")
    gap = (dw - 4 * tile) // 5
    x = (dx + gap + tile + gap // 2) * K    # between the first two icons
    j = 6                                   # panel rows below the top
    app.home()
    w, h, at = app.screen()
    flat = at(x, dy * K + j)
    q.tap(*[c + m("MOB_DOCK_TILE") // 2 for c in dock_xy(2, 4)])
    app.said("[mobile] Settings (dock)")
    page(app, "look")
    q.tap(180, row_y(LOOK["glass"]))
    app.said("[settings] theme Liquid Glass")
    # Settings itself turns to glass: its page the wallpaper, dimmed and
    # sharp, its card frosted glass over that, as Weft's toolbar is over
    # a page - here, by a row with nothing on its right, the wallpaper
    # under the card blurred, lifted and tinted
    time.sleep(1)
    w, h, at = app.screen()
    below = (180 * K, 560 * K)
    card = ((LW - 60) * K, row_y(LOOK["matte"]) * K)
    got_below, got_card = at(*below), at(*card)
    want_below = dimmed_by_glass(wallpaper_px(1, below[1], h))
    rows = len(LOOK)                        # the card's rows
    top = a("SET_CARD_Y") * K
    col = frosted([dimmed_by_glass(wallpaper_px(1, top + j, h))
                   for j in range(rows * a("SET_ROW_H") * K)])
    want_card = tint(lift(col[card[1] - top]))
    check(f"with Liquid Glass Settings' page is the wallpaper dimmed "
          f"({got_below}, {want_below} worked out), its card frosted glass "
          f"over it ({got_card}, {want_card} worked out)",
          got_below == want_below and got_card == want_card)
    app.home()
    home = app.screen()
    w, h, at = home
    bent = at(x, dy * K + j)
    want_flat = matte(wallpaper_px(1, dy * K + j, h))
    want_bent = glass_rim(frosted([wallpaper_px(1, dy * K + row, h)
                                   for row in range(dh * K)]), j)
    check(f"matte: the dock is white over the wallpaper ({flat}, "
          f"{want_flat} worked out); Liquid Glass: the wallpaper frosted, "
          f"bent from further in, each colour by its own amount, brighter, "
          f"tinted and lit by the bevel ({bent}, {want_bent} worked out)",
          flat == want_flat and bent == want_bent)
    # The rim's glint: a panel pixel inside the rim where the top-left
    # corner faces the light square on, and the same at the top-right
    # corner, which faces across it, and on the straight top edge. The
    # Ocean's red is near 0 there: only the glint makes it light.
    pr = m("MOB_DOCK_R") * K
    c = next(c for c in range(pr) if math.isqrt(2 * c * c) == pr - 1)
    tl = at(dx * K + pr - c, dy * K + pr - c)
    tr = at((dx + dw) * K - 1 - pr + c, dy * K + pr - c)
    top = at((dx + dw // 2) * K, dy * K + 1)
    check(f"the rim glints where the corner facing the light turns "
          f"({tl}), not at the corner across from it ({tr}) nor along "
          f"the top edge ({top})",
          tl[0] >= tr[0] + 40 and tl[0] >= top[0] + 40)
    # The shade over glass: what is under it blurred
    q.touch([(180, 8), (180, 150), (180, 300)])
    app.said("[mobile] shade (opened)")
    w, h, at = app.screen()
    px, py = 180 * K, 520 * K
    got = at(px, py)
    want = backdrop(home, px, py)
    q.touch([(180, 500), (180, 350), (180, 200)])
    app.said("[mobile] shade (closed)")
    check(f"with Liquid Glass the shade's backdrop is home blurred - in "
          f"blocks of {m('BLUR_BLOCK')}, smoothed, drawn back between them, "
          f"darkened ({got}, {want} worked out)", got == want)


def backdrop(screen, px, py):
    """mobile.c's backdrop_blur at one panel pixel, from the screen
    it blurs"""
    w, h, at = screen
    B = m("BLUR_BLOCK")
    sw, sh = (w + B - 1) // B, (h + B - 1) // B
    small = {}
    by0, bx0 = py // B, px // B

    def block(bx, by):
        if (bx, by) not in small:
            acc, c = [0, 0, 0], 0
            for y in range(by * B, min((by + 1) * B, h)):
                for x in range(bx * B, min((bx + 1) * B, w)):
                    p = at(x, y)
                    for i in range(3):
                        acc[i] += p[i]
                    c += 1
            small[(bx, by)] = tuple(v // c for v in acc)
        return small[(bx, by)]

    def box(get, x, y, across):
        acc, c = [0, 0, 0], 0
        for d in range(-2, 3):
            xx, yy = (x + d, y) if across else (x, y + d)
            if 0 <= xx < sw and 0 <= yy < sh:
                v = get(xx, yy)
                for i in range(3):
                    acc[i] += v[i]
                c += 1
        return tuple(v // c for v in acc)

    def smooth(x, y):
        return box(lambda xx, yy: box(block, xx, yy, True), x, y, False)

    fy = (py * 256 + 128) // B - 128
    y0, ty = (0, 0) if fy < 0 else (fy >> 8, fy & 0xFF)
    y1 = y0 + 1 if y0 + 1 < sh else y0
    fx = (px * 256 + 128) // B - 128
    x0, tx = (0, 0) if fx < 0 else (fx >> 8, fx & 0xFF)
    x1 = x0 + 1 if x0 + 1 < sw else x0
    a, b = smooth(x0, y0), smooth(x1, y0)
    c, d = smooth(x0, y1), smooth(x1, y1)
    out = []
    for i in range(3):
        top = a[i] * (256 - tx) + b[i] * tx
        bot = c[i] * (256 - tx) + d[i] * tx
        out.append(((top * (256 - ty) + bot * ty) >> 16) * 3 // 4)
    return tuple(out)


def display_size(app):
    """Small, 150%: the logical screen 480 x 853, drawn at 1.5 panel
    pixels to one, touched at the new size's places"""
    q = app.q
    app.home()
    q.tap(*[c + m("MOB_DOCK_TILE") // 2 for c in dock_xy(2, 4)])
    app.said("[mobile] Settings (dock)")
    page(app, "display")
    q.tap(180, row_y(DISPLAY["size"]))
    app.said("[settings] Display size")
    out = None
    q.tap(180, row_y(2))
    out = app.said("[mobile] display size")
    m_ = re.search(r"display size (\d+)% \((\d+)x(\d+)\)", out)
    size = tuple(int(g) for g in m_.groups()) if m_ else None
    check(f"the display size Small is 150%, the screen 480 x 853 logical "
          f"({size})", size == (150, 480, 853))
    q.LOGICAL = (480, 853)
    seen = len(q.serial())
    q.touch([(240, 845), (240, 700), (240, 560)])
    q.wait_serial("[mobile] home", timeout=20)
    w, h, at = app.screen()
    gx, gy, tile = m("MOB_GRID_X"), m("MOB_GRID_Y"), m("MOB_TILE")
    k = 1.5
    first = at(int((gx + 4) * k), int((gy + tile // 2) * k))
    want = rgb(array_colours(MOB, "apps")[0])
    check(f"home's first tile is where 1.5 to one puts it ({first}, its "
          f"colour {want})", first == want)
    x, y = dock_xy(1, 4, 480, 853)
    q.tap(x + m("MOB_DOCK_TILE") // 2, y + m("MOB_DOCK_TILE") // 2)
    out = q.wait_serial("[mobile] Calculator", timeout=20)[seen:]
    check("a tap at the new size's place of the dock's Calculator opens it",
          "[mobile] Calculator (dock)" in out)


def main():
    print("--- the phone's dock, shade, settings and apps ---")
    why = noarm64()
    if why:
        print("SKIPPED: " + why)
        return summary()
    if not os.path.exists(NOTETEST):
        print("SKIPPED: no ARM64 initrd's programs (make kernelarm64)")
        return summary()
    work = tempfile.mkdtemp()
    pkg = os.path.join(work, "notes.tapp")
    subprocess.run([sys.executable, os.path.join(ROOT, "scripts",
                                                 "mktapp.py"), pkg,
                    "--id", "notetest", "--name", "Notifier", "--version",
                    "1", "--arch", "aarch64", "--exec", "notetest",
                    "notetest=" + NOTETEST], check=True, capture_output=True)
    ptest.RTC_BASE = "2030-06-13T15:34:10"
    q = Phone(cores=2, mib=1024, extra=[
        "-fw_cfg", "name=opt/threados/battery,string=73 discharging",
        "-fw_cfg", "name=opt/threados/apps/notetest,file=" + pkg,
        "-netdev", "user,id=n0", "-device", "virtio-net-device,netdev=n0"])
    app = App(q)
    try:
        q.wait_serial("[notify] Network: Connected, 10.0.2.15", timeout=120)
        q.wait_serial("[mobile] lock screen", timeout=60)
        ly = m("MOB_LOCK_NOTE_Y")
        title = app.read(28, LW - 20, ly + 24, 15, face="bold")
        body = app.read(28, LW - 20, ly + 44, 13, fg=MUTED)
        check(f"the lock screen lists the network's notification "
              f"({title!r}: {body!r})",
              title == "Network" and body == "Connected, 10.0.2.15")
        q.touch([(180, 560), (180, 420), (180, 300)])
        app.said("[mobile] home (swipe up)")
        scr = app.screen()
        bars = app.bars(scr)
        check(f"the status bar: the network's four bars ({len(bars)})",
              len(bars) == 4 and
              all(b[0] - a_[0] == m("MOB_BAR_PITCH") * K
                  for a_, b in zip(bars, bars[1:])))
        want = array_colours(MOB, "dock") + [0xFF9F0A]
        got = []
        for i in range(4):
            x, y = dock_xy(i, 4)
            got.append(app.at(x + 4, y + m("MOB_DOCK_TILE") // 2, scr))
        check(f"the dock: Notes, Calculator, Settings, the package - each "
              f"its colour ({got})", got == [rgb(c) for c in want])

        q.tap(*[c + m("MOB_DOCK_TILE") // 2 for c in dock_xy(1, 4)])
        app.said("[mobile] Calculator (dock)")
        calculator(app)
        app.home()
        q.tap(*[c + m("MOB_DOCK_TILE") // 2 for c in dock_xy(0, 4)])
        app.said("[mobile] Notes (dock)")
        notes(app)
        app.home()
        settings(app)
        shade(app)
        navigation(app)
        glass(app)
        display_size(app)
    finally:
        q.close()
        subprocess.run(["rm", "-rf", work])
    return summary()


if __name__ == "__main__":
    sys.exit(main())
