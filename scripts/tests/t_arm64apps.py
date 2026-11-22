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
  (no pill; back, recent apps, home); the thread (a knot for each
  program opened, in order, each its colour, the one in front ringed
  with its colour along the line; a tap on a knot, a finger slid along
  it, a swipe up for home, up and held for recent apps, in from the
  left edge for back; the knots one colour when their colours are
  off); a PIN (chosen twice, two digits tying two knots in the accent,
  a different second refused, then asked for after the lock screen's
  swipe - a wrong one refused, the right one opening the phone - and
  the swipe alone again); the display size at 150% (the
  screen 480 x 853, the grid's tile where that scale puts it, and a tap
  at the new size's places reaching the dock)

  the shade, as an iPhone's Control Center: pulled down from the status
  bar; the flashlight's round button switches it on and off; the volume
  slider takes the value where it is touched, the brightness slider
  follows a finger down and up past its top without the shade taking
  that for a swipe that closes it; the turn button lays the phone on
  its side and back; with Focus (do not disturb) on, the program's
  notification makes no banner but is first in the list, the network's
  after it; off, it makes a banner; Clear empties the list; the
  calculator's round button opens the Calculator
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

# mobile.c's Control Center: its controls in order, and the grid cells of
# those past the square of four round buttons
CC_ORDER = ["airplane", "network", "dark", "saver", "bright", "volume",
            "dnd", "turn", "torch", "calc", "rec", "notes", "settings"]
CC_CELLS = {"bright": (2, 0, 1, 2), "volume": (3, 0, 1, 2),
            "dnd": (0, 2, 2, 1), "turn": (2, 2, 1, 1), "torch": (3, 2, 1, 1),
            "calc": (0, 3, 1, 1), "rec": (1, 3, 1, 1), "notes": (2, 3, 1, 1),
            "settings": (3, 3, 1, 1)}


def cc_unit(lw=LW, lh=LH):
    gap = m("CC_GAP")
    across = (min(lw - 2 * m("CC_SIDE"), m("CC_MAXW")) - 3 * gap) // 4
    down = (lh - m("CC_TOP") - m("CC_FOOT") - 3 * gap) // 4
    return min(across, down)


def cc_left(lw=LW, lh=LH):
    width = 4 * cc_unit(lw, lh) + 3 * m("CC_GAP")
    return m("CC_SIDE") if lw > lh else (lw - width) // 2


def cc_box(name, lw=LW, lh=LH):
    """mobile.c's cc_box: x, y, w, h of a control, logical pixels"""
    u, gap, top, pad = cc_unit(lw, lh), m("CC_GAP"), m("CC_TOP"), m("CC_PAD")
    left, i = cc_left(lw, lh), CC_ORDER.index(name)
    if i < 4:
        d = (2 * u + gap - 3 * pad) // 2
        return (left + pad + (i % 2) * (d + pad),
                top + pad + (i // 2) * (d + pad), d, d)
    col, row, cols, rows = CC_CELLS[name]
    return (left + col * (u + gap), top + row * (u + gap),
            cols * u + (cols - 1) * gap, rows * u + (rows - 1) * gap)


def said_after(q, seen, text, timeout=20):
    """The serial port's text since 'seen', once 'text' is in it (or as
    it is when the time is up)"""
    end = time.time() + timeout
    while time.time() < end:
        out = q.serial()[seen:]
        if text in out:
            return out
        time.sleep(0.3)
    return q.serial()[seen:]


def cc_xy(name, lw=LW, lh=LH):
    x, y, w, h = cc_box(name, lw, lh)
    return x + w // 2, y + h // 2


def notes_area(lw=LW, lh=LH):
    """mobile.c's notes_area, upright: the heading's baseline, the cards'
    left and the first one's top"""
    width = 4 * cc_unit(lw, lh) + 3 * m("CC_GAP")
    w = min(lw - 24, width + 24)
    head = m("CC_TOP") + width + m("CC_NOTES_DOWN")
    return head, (lw - w) // 2, head + m("CC_CARDS_DOWN")


def shade(app):
    q = app.q
    app.home()
    q.touch([(180, 8), (180, 150), (180, 300)])
    app.said("[mobile] shade (opened)")
    q.tap(*cc_xy("dnd"))                             # Focus: do not disturb
    app.said("[mobile] do not disturb on")
    q.tap(*cc_xy("torch"))
    app.said("[mobile] flashlight on")
    q.tap(*cc_xy("torch"))
    app.said("[mobile] flashlight off")
    # the volume where the finger touches its slider, a quarter up
    x, y, w, h = cc_box("volume")
    tap_y = y + h - h // 4
    seen = len(q.serial())
    q.tap(x + w // 2, tap_y)
    vol = (y + h - tap_y) * 100 // h
    out = said_after(q, seen, "[mobile] volume ")
    check(f"the shade's volume slider: a quarter of the way up is "
          f"{vol}% ({said_line(out, '[mobile] volume')})",
          f"[mobile] volume {vol}%" in out)
    # the brightness dragged down and back up past its top: the finger
    # goes up, as a swipe that closes the shade would, but began on the
    # slider; the screen as bright as it was for what follows
    x, y, w, h = cc_box("bright")
    seen = len(q.serial())
    q.touch([(x + w // 2, y + 20 + i * (h - 40) // 6) for i in range(7)])
    low_y = y + 20 + 6 * (h - 40) // 6
    low = max(10, (y + h - low_y) * 100 // h)
    out = said_after(q, seen, f"[mobile] brightness {low}%")
    seen2 = len(q.serial())
    q.touch([(x + w // 2, low_y - i * (h - 20) // 6) for i in range(7)])
    full = said_after(q, seen2, "[mobile] brightness 100%")
    check(f"the brightness slider follows the finger, down to {low}% and "
          f"up to 100%, the shade still open",
          f"[mobile] brightness {low}%" in out and
          "[mobile] brightness 100%" in full and
          "shade (closed)" not in q.serial()[seen:])
    # the phone turned on its side from the shade, its controls laid
    # out for that, and turned back
    seen = len(q.serial())
    q.tap(*cc_xy("turn"))
    out = said_after(q, seen, "[mobile] landscape")
    q.LOGICAL = (LH, LW)
    time.sleep(1)
    seen = len(q.serial())
    q.tap(*cc_xy("turn", LH, LW))
    back_up = said_after(q, seen, "[mobile] portrait")
    q.LOGICAL = (LW, LH)
    time.sleep(1)
    check(f"the shade's phone turns it on its side "
          f"({said_line(out, '[mobile] landscape')}) and back upright "
          f"({said_line(back_up, '[mobile] portrait')})",
          f"[mobile] landscape, {LH}x{LW}" in out and
          f"[mobile] portrait, {LW}x{LH}" in back_up)
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
    head, cx, cy = notes_area()
    step = m("MOB_SHADE_CARD_STEP")
    scr = app.screen()
    cards = [(app.read(cx + 16, LW - 60, cy + i * step + 24, 15, face="bold",
                       scr=scr),
              app.read(cx + 16, LW - 20, cy + i * step + 44, 13, fg=MUTED,
                       scr=scr)) for i in range(2)]
    check(f"but the shade lists it first, the network's after it "
          f"({cards})", cards == [("Test", "Hello from a program"),
                                  ("Network", "Connected, 10.0.2.15")])
    q.tap(*cc_xy("dnd"))
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
    q.tap(LW - 40, head - 6)
    app.said("[mobile] notifications cleared")
    empty = app.read(0, LW, cy + 34, 15, fg=MUTED)
    check(f"Clear empties the list ({empty!r})", empty == "No notifications")
    # an app from the shade's round buttons
    seen = len(q.serial())
    q.tap(*cc_xy("calc"))
    out = said_after(q, seen, "[mobile] Calculator (the shade)")
    check("the shade's calculator button opens the Calculator",
          "[mobile] Calculator (the shade)" in out)
    app.home()


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


def app_colours():
    """mobile.c's apps and dock: each screen's name and colour"""
    text = open(os.path.join(ROOT, MOB)).read()
    out = {}
    for name, c in re.findall(r'\{ "([^"]+)",\s*\w+,\s*0x([0-9A-Fa-f]{6}) \}',
                              text):
        out[name] = rgb(int(c, 16))
    v = re.search(r"#define\s+MOB_WIN_TILE\s+0x([0-9A-Fa-f]{6})", text)
    out["window"] = rgb(int(v.group(1), 16))
    return out


def last_thread(q):
    """The knots on the thread, as the shell last said them"""
    lines = [l for l in q.serial().splitlines()
             if l.startswith("[mobile] thread:")]
    if not lines:
        return []
    rest = lines[-1][len("[mobile] thread:"):].strip()
    return [n.strip() for n in rest.split(",")] if rest else []


def knot_xs(n, front):
    """mobile.c's knot_x: the knots in a row MOB_KNOT_GAP apart edge to
    edge, the one in front (MOB_KNOT_FRONT across) in the screen's
    middle"""
    at, mids = 0, []
    for k in range(n):
        d = m("MOB_KNOT_FRONT") if k == front else m("MOB_KNOT")
        mids.append(at + d // 2)
        at += d + m("MOB_KNOT_GAP")
    return [LW // 2 + x - mids[front] for x in mids]


def weave_dyes():
    """The six dyes, Night's, as docs/WEAVE.md tabulates them"""
    text = open(os.path.join(ROOT, "docs", "WEAVE.md")).read()
    return {n: rgb(int(v, 16)) for n, v in re.findall(
        r"\| \*\*(\w+)\*\* \|[^|]*\| `#([0-9A-Fa-f]{6})` \|", text)}


def design_thread_line():
    """The thread's line on the dark phone, as NavStyles.dc.html draws it"""
    text = open(os.path.join(ROOT, "docs", "weave", "canvas",
                             "NavStyles.dc.html")).read()
    v = re.search(r"top: 10px; height: 2px; background: #([0-9A-Fa-f]{6})",
                  text)
    return rgb(int(v.group(1), 16))


def thread_nav(app):
    """The thread (docs/weave/canvas/NavStyles.dc.html): a knot a program
    along the bottom - tapped, slid along, up for home, up and held for
    recent apps, in from the left edge for back"""
    q = app.q
    dyes = weave_dyes()
    grey = rgb(int(re.search(r"#define\s+NIGHT_APP_DIM\s+0x([0-9A-Fa-f]{6})",
                             open(os.path.join(ROOT, "kernel", "include",
                                               "look.h")).read()).group(1),
                   16))
    want = {"Notes": dyes["Saffron"], "Calculator": dyes["Weld"],
            "Settings": grey}
    q.tap(*[c + m("MOB_DOCK_TILE") // 2 for c in dock_xy(2, 4)])
    app.said("[mobile] Settings (dock)")
    page(app, "navigation")
    q.tap(180, row_y(2))
    app.said("[settings] The thread, ThreadOS's own")
    knots = last_thread(q)
    n = len(knots)
    check(f"the thread holds the programs opened, in order ({knots})",
          n >= 3 and "Settings" in knots and "Notes" in knots and
          "Calculator" in knots and knots.index("Notes") >
          knots.index("Calculator"))
    if n < 3 or "Settings" not in knots:
        return
    front = knots.index("Settings")
    xs = knot_xs(n, front)
    ky = (LH - m("MOB_THREAD_LINE") + 1) * K       # the knots' middle row
    w, h, at = app.screen()
    line = at(24 * K, (LH - m("MOB_THREAD_LINE")) * K)
    check(f"the thread's line is the design's ({line}, "
          f"{design_thread_line()})", line == design_thread_line())
    mid = at(xs[front] * K, ky)
    ring = (at(xs[front] * K - 12, ky), at(xs[front] * K + 12, ky))
    # (the band's own pixels: between the knot before and the front's
    # ring on the left, past the ring on the right)
    band = (at((LW // 2 - 12) * K, ky), at((LW // 2 + 20) * K, ky))
    check(f"Settings, in front, stands in the middle ({xs[front]}): a knot "
          f"of the system's grey ({mid}) ringed in white ({ring}) on its "
          f"colour along the line, either side of the middle ({band})",
          xs[front] == LW // 2 and mid == grey and
          ring == (WHITE, WHITE) and band == (grey, grey))
    others = [(knots[i], at(xs[i] * K, ky)) for i in range(n) if i != front]
    check(f"the others are knots of their dyes, as docs/WEAVE.md has them "
          f"- Notes Saffron, Calculator Weld ({others})",
          all(c == want.get(name, c) for name, c in others) and
          dict(others).get("Notes") == dyes["Saffron"] and
          dict(others).get("Calculator") == dyes["Weld"])

    # a tap on a knot: that program, which then stands in the middle
    t = knots.index("Notes")
    seen = len(q.serial())
    q.tap(xs[t], LH - 17)
    out = said_after(q, seen, "[mobile] Notes (its knot)")
    check("a tap on Notes' knot brings Notes to the front",
          "[mobile] Notes (its knot)" in out)
    w, h, at = app.screen()
    xs = knot_xs(n, t)
    got = at(LW // 2 * K, ky)
    check(f"and Notes' knot moves to the middle ({got})",
          got == dyes["Saffron"])
    # sliding along it: a knot each MOB_KNOT_STEP, under the middle
    c = knots.index("Calculator")
    seen = len(q.serial())
    dx = (c - t) * m("MOB_KNOT_STEP")
    path = [(LW // 2 + k * dx // 6, LH - 15) for k in range(7)]
    q.touch(path)
    out = said_after(q, seen, "[mobile] Calculator (along the thread)")
    check(f"a finger slid {abs(dx)} pixels along the thread brings "
          f"Calculator, {abs(c - t)} knot away",
          "[mobile] Calculator (along the thread)" in out)
    # up: home; up and held: recent apps
    seen = len(q.serial())
    q.touch([(180, LH - 12), (180, LH - 60), (180, LH - 140)])
    out = said_after(q, seen, "[mobile] home (up from the thread)")
    check("a swipe up from the thread goes home",
          "[mobile] home (up from the thread)" in out)
    seen = len(q.serial())
    q.touch([(180, LH - 12), (180, LH - 60), (180, LH - 140)], hold=1.0)
    out = said_after(q, seen, "[mobile] recent apps (up and held")
    check("up and held there opens recent apps",
          "[mobile] recent apps (up and held on the thread)" in out)
    # in from the left edge: back
    q.touch([(180, LH - 12), (180, LH - 60), (180, LH - 140)])
    app.said("[mobile] home (up from the thread)")
    q.tap(*[c_ + m("MOB_DOCK_TILE") // 2 for c_ in dock_xy(2, 4)])
    app.said("[mobile] Settings (dock)")
    page(app, "navigation")
    seen = len(q.serial())
    q.touch([(4, 300), (40, 300), (90, 300), (150, 300)])
    out = said_after(q, seen, "[settings] Settings")
    check("in from the left edge is back: Settings a page up",
          "[mobile] back (the left edge)" in out and
          "[settings] Settings" in out)
    # the knots all one colour, the one in front the accent's
    page(app, "navigation")
    q.tap(SWITCH_X, row_y(3))
    app.said("[settings] Knots in programs' colours off")
    w, h, at = app.screen()
    xs = knot_xs(n, front)
    plain = [at(xs[i] * K, ky) for i in range(n) if i != front]
    check(f"with their colours off, the knots are all the grey ({plain})",
          plain and all(p == grey for p in plain))
    q.tap(SWITCH_X, row_y(3))
    app.said("[settings] Knots in programs' colours on")
    q.tap(180, row_y(0))
    app.said("[settings] Gestures, as an iPhone")
    app.home()


def pin_lock(app):
    """The PIN (docs/weave/canvas/PhoneUnlock.dc.html): six digits chosen
    twice in Settings, then asked for after the lock screen's swipe - six
    knots on a thread, each digit tying one"""
    q = app.q
    step, key = m("PIN_KEY_STEP"), m("PIN_KEY")

    def press(digits):
        for ch in digits:
            d = int(ch)
            r, c = (3, 1) if d == 0 else ((d - 1) // 3, (d - 1) % 3)
            q.tap(LW // 2 + (c - 1) * step,
                  m("PIN_KEYS_Y") + r * m("PIN_ROW_STEP") + key // 2)

    def hint():
        return app.read(0, LW, m("PIN_HINT_Y"), 13, fg=rgb(m_hex("PIN_DIM")))

    q.tap(*[c + m("MOB_DOCK_TILE") // 2 for c in dock_xy(2, 4)])
    app.said("[mobile] Settings (dock)")
    page(app, "display")
    q.tap(180, row_y(4))
    app.said("[settings] Screen lock")
    q.tap(180, row_y(1))
    app.said("[mobile] PIN (Settings)")
    h0 = hint()
    check(f"choosing a PIN opens its keys ('{h0}')", h0 == "Choose a PIN")
    press("48")
    app.said("[mobile] PIN 2 of 6")
    w, h, at = app.screen()
    kw, kn = m("PIN_KNOTS_W"), m("PIN_KNOT")
    kx = [(LW - kw) // 2 + kn // 2 + i * (kw - kn) // 5 for i in range(6)]
    ky = m("PIN_KNOTS_Y") * K
    bg, off = rgb(m_hex("PIN_BG")), rgb(m_hex("PIN_RING_OFF"))
    tied = [at(kx[i] * K, ky) for i in range(2)]
    nxt = (at(kx[2] * K, ky), at(kx[2] * K + 14, ky))
    rest = (at(kx[3] * K, ky), at(kx[3] * K + 14, ky))
    line = (at((kx[0] + kx[1]) // 2 * K, ky - 1),
            at((kx[3] + kx[4]) // 2 * K, ky - 1))
    check(f"two digits tie two knots in the accent ({tied}), the thread "
          f"that far in it ({line[0]}), the next knot ringed in white "
          f"({nxt}), the rest dark ({rest}), the thread on dark "
          f"({line[1]})",
          tied[0] == tied[1] and tied[0] not in (bg, WHITE, off) and
          line[0] == tied[0] and nxt == (bg, WHITE) and rest == (bg, off)
          and line[1] == rgb(m_hex("PIN_TRACK")))
    press("2915")
    h1 = hint()
    check(f"six, and it asks again ('{h1}')", h1 == "Again, to be sure")
    press("111111")
    app.said("[mobile] the PINs differ")
    h2 = hint()
    check(f"another six: not the same, choose again ('{h2}')",
          h2 == "Not the same - choose again")
    seen = len(q.serial())
    press("482915")
    press("482915")
    out = said_after(q, seen, "[settings] PIN set")
    check("the same six twice: the PIN is set, back in Settings",
          "[settings] PIN set" in out and
          "[mobile] Settings (the PIN chosen)" in out)
    q.tap(180, row_y(2))
    app.said("[mobile] lock screen (Lock now)")
    seen = len(q.serial())
    q.touch([(180, 560), (180, 420), (180, 300)])
    out = said_after(q, seen, "[mobile] PIN (swipe up)")
    check("locked, the swipe up asks for the PIN, not home",
          "[mobile] PIN (swipe up)" in out and "[mobile] home" not in out)
    h3 = hint()
    press("000000")
    app.said("[mobile] PIN wrong")
    h4 = hint()
    check(f"a wrong one is refused ('{h3}' -> '{h4}')",
          h3 == "Your PIN" and h4 == "Wrong PIN - again" and
          "[mobile] home" not in q.serial()[seen:])
    seen = len(q.serial())
    press("482915")
    out = said_after(q, seen, "[mobile] home (the PIN)")
    check("the right one opens the phone",
          "[mobile] PIN right" in out and "[mobile] home (the PIN)" in out)
    # back to the swipe alone
    q.tap(*[c + m("MOB_DOCK_TILE") // 2 for c in dock_xy(2, 4)])
    app.said("[mobile] Settings (dock)")
    page(app, "display")
    q.tap(180, row_y(4))
    app.said("[settings] Screen lock")
    q.tap(180, row_y(0))
    app.said("[settings] Swipe up")
    q.tap(180, row_y(2))
    app.said("[mobile] lock screen (Lock now)")
    seen = len(q.serial())
    q.touch([(180, 560), (180, 420), (180, 300)])
    out = said_after(q, seen, "[mobile] home (swipe up)")
    check("with the swipe alone again, the swipe opens it",
          "[mobile] home (swipe up)" in out)


def m_hex(name):
    v = re.search(r"#define\s+%s\s+0x([0-9A-Fa-f]{6})" % name,
                  open(os.path.join(ROOT, MOB)).read())
    return int(v.group(1), 16)


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
    px, py = 6 * K, 520 * K                 # beside the controls and cards
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
    seen = len(q.serial())
    q.tap(180, row_y(2))
    out = said_after(q, seen, "[mobile] display size")
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
        thread_nav(app)
        pin_lock(app)
        glass(app)
        display_size(app)
    finally:
        q.close()
        subprocess.run(["rm", "-rf", work])
    return summary()


if __name__ == "__main__":
    sys.exit(main())
