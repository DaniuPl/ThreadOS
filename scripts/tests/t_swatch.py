#!/usr/bin/env python3
"""ThreadOS smoke test: Swatch, ProtonUI v2's own catalogue.

user64/gswatch.c (docs/weave/canvas/Swatch.dc.html): down the left
every control user/ugui.h has, in the middle the chosen one in its
states as the toolkit draws them and one to try, under them the lines
of C that make it and what it sends; Night or Linen for the preview; on
the right, Start a program. Read off the screen under Weave - Inter and
the mono face through FreeType, colours against ugui.h's own numbers -
and the serial port:

  the list is the catalogue in gswatch.c, in the mono face, the chosen
  one (pu_switch) marked
  pu_switch's heading, its four states - off, on in the accent, focused
  with the thread round it, unavailable at two fifths - and its first
  line of C, as gswatch.c writes it
  the one to try, clicked, says PU_CHANGED 0 and is off on the screen
  pu_button: pressed in ugui.h's pressed indigo; pu_field: wrong, its
  stitch madder and why under it; pu_card, pu_avatar, pu_side drawn
  Linen: the preview's ground is Linen's, the window's still Night's
  pu_menu: a right click on the one to try opens the toolkit's menu,
  and its second line comes back as PU_CHOSEN 1
  pu_sheet: the question over the window with a madder selvedge and
  its title; its madder yes comes back as PU_PRESSED 1
  Start a program: the Settings-like template, Make it - the project in
  ~/Projects with its main.c from the template, named, first among
  Code's projects, and Code running

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, LOOK, _kernel_define

L = LOOK
N = theme("night")
SRC = "user64/gswatch.c"
UG = "user/ugui.h"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(
    __file__))))
WHITE = (255, 255, 255)


def d(name, src=SRC):
    return _kernel_define(src, name)


def hexdef(name, src=UG):
    v = re.search(r"#define\s+%s\s+0x([0-9A-Fa-f]{6})" % name,
                  open(os.path.join(ROOT, src)).read())
    c = int(v.group(1), 16)
    return ((c >> 16) & 255, (c >> 8) & 255, c & 255)


def catalogue():
    """gswatch.c's entries: (name, first line of code)"""
    text = open(os.path.join(ROOT, SRC)).read()
    body = text[text.index("static const struct entry cat[]"):]
    body = body[:body.index("\n};")]
    out = []
    for m in re.finditer(r'\{ "(pu_\w+)",\s*\{ "((?:[^"\\]|\\.)*)"', body):
        out.append((m.group(1), m.group(2).replace('\\"', '"')))
    return out


def mix(over, under, a):
    return tuple((o * a + u * (255 - a) + 127) // 255
                 for o, u in zip(over, under))


def near(a, b, tol=6):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def main():
    print("--- Swatch ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            run(q)
        finally:
            q.close()
    finally:
        for p in (disk, slog):
            if os.path.exists(p):
                os.remove(p)
    return summary()


def run(q):
    check("the desktop came up", q.wait_desktop())
    W, H = q.size()
    q.click_desktop()
    check("'layout weave'", "saved" in q.sudo("set layout weave", "saved"))
    end = time.time() + 10
    while time.time() < end and "the Weave style" not in q.serial():
        time.sleep(0.3)
    q.type_line("runw gswatch")
    out = q.wait_serial("[swatch] drawn", timeout=60)
    m = re.search(r"\[swatch\] drawn (\d+) (\d+)", out)
    cw, ch = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
    check(f"Swatch opens at its smaller size on this screen ({cw}x{ch})",
          (cw, ch) == (d("W0"), d("H0")))
    time.sleep(1.5)
    away = (W - 4, H // 2)
    q.mouse_goto(*away)

    # its content's corner: under the selvedge, the window's thread colour
    _, _, at = q.screen_raw()
    acc = N["accent"]
    sel = None
    for y in range(0, 200):
        for x in range(0, W - 100, 2):
            if at(x, y) == acc and at(x + 99, y) == acc and \
                    at(x, y + 2) == acc and at(x, y + 3) != acc:
                sel = (x, y)
                break
        if sel:
            break
    check(f"its window, with the selvedge ({sel})", sel)
    if not sel:
        return
    ox = sel[0] - L["WEAVE_WIN_R"] + 1
    oy = sel[1] + L["WEAVE_TITLE_H"] + 1
    side_x, side_y, side_w = d("SIDE_X"), d("SIDE_Y"), d("SIDE_W")
    row = _kernel_define(UG, "PU_SIDE_ROW")
    px = d("SIDE_X") + d("SIDE_W") + d("GAP")
    rx = cw - d("RIGHT_W") - d("GAP")
    pw = rx - d("GAP") - px
    ph = ch - d("CODE_H")

    def side_name(i, scr=None):
        return q.read_ui(ox + side_x + 26,
                         ui_font_baseline_in(12, oy + side_y + i * row, row),
                         side_w - 30, px=12, face="jbmono", screen=scr)

    def choose(i, name):
        seen = len(q.serial())
        q.click(ox + side_x + 60, oy + side_y + i * row + row // 2)
        q.mouse_goto(*away)
        end = time.time() + 10
        while time.time() < end and \
                "[swatch] chose %s" % name not in q.serial()[seen:]:
            time.sleep(0.2)
        time.sleep(0.8)
        return "[swatch] chose %s" % name in q.serial()[seen:]

    cat = catalogue()
    scr = q.screen_raw()
    names = [side_name(i, scr) for i in range(len(cat))]
    check(f"down the left, the catalogue in the mono face ({names})",
          names == [n for n, _ in cat] and len(names) >= 12)
    _, _, at = scr
    marked = at(ox + side_x + 10 + 3, oy + side_y + 2 * row + row // 2)
    check(f"pu_switch chosen first, its knot in the accent ({marked})",
          near(marked, acc, 8))

    # pu_switch: the heading, the four states, the code
    head = q.find_ui(ox + px, ox + px + pw, oy + d("HEAD_Y"), px=12,
                     face="interb", screen=scr)
    check(f"the heading ({head!r})", head == "PU_SWITCH - STATES")
    slot_w, sy = d("SLOT_W"), d("STATE_Y")
    sw, sh = _kernel_define(UG, "PU_SWITCH_W"), _kernel_define(UG,
                                                                "PU_SWITCH_H")
    x0 = px + (pw - 4 * slot_w) // 2
    tracks, knobs = [], []
    for k in range(4):
        sx = x0 + k * slot_w + (slot_w - sw) // 2
        ty = sy + (sh + 4 - sh) // 2
        tracks.append(at(ox + sx + 20, oy + ty + 3))
        knobs.append(at(ox + sx + 4 + 7, oy + ty + 4 + 7))
    ground = at(ox + px + 5, oy + 30)
    ring = at(ox + x0 + 2 * slot_w + (slot_w - sw) // 2 - 4,
              oy + sy + 2 + sh // 2)
    seg_on, knob_off = hexdef("PU_N_SEG_ON"), hexdef("PU_N_KNOB_OFF")
    check(f"pu_switch's states: off ({tracks[0]}), on in the accent "
          f"({tracks[1]}), focused with the thread round it ({ring}), "
          f"unavailable at two fifths ({knobs[3]} = {knob_off} over "
          f"{ground} at 102)",
          near(tracks[0], seg_on, 8) and near(tracks[1], acc, 8) and
          near(ring, acc, 8) and
          near(knobs[3], mix(knob_off, ground, 102), 6) and
          near(knobs[0], knob_off, 6))
    code0 = q.read_ui(ox + px + 14, oy + ph + 24, pw - 20, px=12,
                      face="jbmono", screen=scr)
    check(f"its first line of C ({code0!r})",
          code0 == dict(cat)["pu_switch"])

    # the one to try
    lx, ly = px + 96, ph - d("TRY_DY") + 8
    lw = min(pw - 120, 220)
    seen = len(q.serial())
    q.click(ox + lx + lw - sw // 2, oy + ly + 2 + sh // 2)
    q.mouse_goto(*away)
    q.wait_serial("[swatch] sent", timeout=10)
    time.sleep(0.6)
    _, _, at = q.screen_raw()
    live = at(ox + lx + lw - sw + 20, oy + ly + 2 + 3)
    check(f"the one to try, clicked, sends PU_CHANGED 0 and is off "
          f"({live})", "[swatch] sent PU_CHANGED 0" in q.serial()[seen:] and
          not near(live, acc, 30))

    # pu_button: pressed
    check("pu_button chosen", choose(0, "pu_button"))
    _, _, at = q.screen_raw()
    x0 = px + (pw - 4 * slot_w) // 2
    rest = at(ox + x0 + 4 + 6, oy + sy + 16)
    pressed = at(ox + x0 + slot_w + 4 + 6, oy + sy + 16)
    check(f"its states: at rest the accent ({rest}), pressed ugui.h's "
          f"pressed indigo ({pressed})",
          near(rest, acc, 8) and near(pressed, hexdef("PU_N_PRESSED"), 8))

    # pu_field: wrong
    check("pu_field chosen", choose(1, "pu_field"))
    scr = q.screen_raw()
    _, _, at = scr
    fx, fy = px + 110, sy + 2 * d("ROW_STEP")
    fh = 24
    stitch = at(ox + fx + 30, oy + fy + fh - 1)
    why = q.read_ui(ox + fx, oy + fy + fh + 15, 220, px=12, face="inter",
                    fg=N["app_danger"], screen=scr)
    check(f"wrong: its stitch madder ({stitch}) and why under it "
          f"({why!r})", near(stitch, N["app_danger"], 10) and
          why == "A port is 1 to 65535")

    # pu_card, pu_avatar, pu_side: drawn
    names_ = [n for n, _ in cat]
    check("pu_card chosen", choose(names_.index("pu_card"), "pu_card"))
    scr = q.screen_raw()
    cx = px + (pw - slot_w) // 2 - 60
    head = q.read_ui(ox + cx + 24, oy + sy - 10 + 24 + 9, 120, px=12,
                     face="interb", screen=scr)
    _, _, at = scr
    card = at(ox + cx + 6, oy + sy + 70)
    check(f"pu_card: its ground ({card}) and its heading in capitals "
          f"({head!r})", near(card, hexdef("PU_N_CARD"), 4) and
          head == "WINDOW")
    check("pu_avatar chosen", choose(names_.index("pu_avatar"), "pu_avatar"))
    _, _, at = q.screen_raw()
    ax = px + (pw - 4 * slot_w) // 2 + slot_w + (slot_w - 48) // 2
    ring_, gap, mid = (at(ox + ax + 1, oy + sy + 24),
                       at(ox + ax + 4, oy + sy + 24),
                       at(ox + ax + 10, oy + sy + 24))
    check(f"pu_avatar: Cochineal, ringed with a gap ({ring_}, {gap}, "
          f"{mid})", near(ring_, (0xD1, 0x52, 0xA6), 30) and
          near(mid, (0xD1, 0x52, 0xA6), 6) and not near(gap, mid, 30))

    # Linen: the preview's ground
    seen = len(q.serial())
    tx = px + pw - d("THEME_W") - 8
    q.click(ox + tx + d("THEME_W") * 3 // 4, oy + 8 + d("THEME_H") // 2)
    q.mouse_goto(*away)
    q.wait_serial("[swatch] theme Linen", timeout=10)
    time.sleep(0.8)
    _, _, at = q.screen_raw()
    g = at(ox + px + 5, oy + 30)
    win_g = at(ox + side_x + side_w // 2, oy + ch - 20)
    check(f"Linen: the preview's ground is Linen's ({g}), the window's "
          f"still Night's ({win_g})",
          near(g, rgb(L["LINEN_WIN_BG"]), 6) and
          not near(win_g, rgb(L["LINEN_WIN_BG"]), 40))
    q.click(ox + tx + d("THEME_W") // 4, oy + 8 + d("THEME_H") // 2)
    q.mouse_goto(*away)
    q.wait_serial("[swatch] theme Night", timeout=10)

    # pu_menu: a right click on the one to try
    check("pu_menu chosen", choose(names_.index("pu_menu"), "pu_menu"))
    seen = len(q.serial())
    q.mouse_goto(ox + lx + 100, oy + ly + 16)
    time.sleep(0.4)
    q.mouse_button(True, "right")
    q.mouse_button(False, "right")
    q.wait_serial("[swatch] sent PU_MENU", timeout=10)
    rh = max(_kernel_define(UG, "PU_TARGET_POINTER"), 28)
    q.click(ox + lx + 100 + 40, oy + ly + 16 + 5 + rh + rh // 2)
    q.mouse_goto(*away)
    q.wait_serial("[swatch] sent PU_CHOSEN", timeout=10)
    check("a right click opens the toolkit's menu; its second line comes "
          "back as PU_CHOSEN 1",
          "[swatch] sent PU_CHOSEN 1" in q.serial()[seen:])

    # pu_sheet: the question over the window
    check("pu_sheet chosen", choose(names_.index("pu_sheet"), "pu_sheet"))
    q.click(ox + lx + 70, oy + ly + 16)
    q.mouse_goto(*away)
    time.sleep(1)
    scr = q.screen_raw()
    _, _, at = scr
    gw = min(cw - 40, _kernel_define(UG, "PU_SHEET_W"))
    gh = _kernel_define(UG, "PU_SHEET_H")
    gx, gy = (cw - gw) // 2, (ch - gh) // 2
    selv = at(ox + gx + gw // 2, oy + gy + 1)
    title = q.read_ui(ox + gx + 20, oy + gy + 36, gw - 40, px=15,
                      face="interb", screen=scr)
    check(f"the question over the window, a madder selvedge ({selv}), its "
          f"title ({title!r})", near(selv, N["app_danger"], 10) and
          title == "Shut down now?")
    bh = 32
    by = gy + gh - bh - 16
    bw = (gw - 40 - 10) // 2
    yes_x = gx + 20 + bw + 10
    yes = at(ox + yes_x + 6, oy + by + bh // 2)
    seen = len(q.serial())
    q.click(ox + yes_x + bw // 2, oy + by + bh // 2)
    q.mouse_goto(*away)
    q.wait_serial("[swatch] sent PU_PRESSED 1", timeout=10)
    check(f"its yes is madder ({yes}) and comes back as PU_PRESSED 1",
          near(yes, N["app_danger"], 10) and
          "[swatch] sent PU_PRESSED 1" in q.serial()[seen:])

    # Start a program
    seen = len(q.serial())
    q.click(ox + rx + 60, oy + d("TPL_Y") + d("TPL_STEP") + 20)
    q.wait_serial("[swatch] template Settings-like", timeout=10)
    q.click(ox + rx + d("RIGHT_W") // 2, oy + ch - 50 + 18)
    q.mouse_goto(*away)
    out = q.wait_serial("[swatch] made", timeout=15)[seen:]
    m = re.search(r"\[swatch\] made (\S+) from Settings-like", out)
    path = m.group(1) if m else ""
    check(f"Make it: the project from Settings-like ({path})",
          path.endswith("/Projects/backup-check"))
    # the keys to the shell: a click on the desktop left of Swatch (the
    # usual place, top right, is under its window)
    q.click(12, H // 2)
    time.sleep(0.5)
    main_c = q.command("cat Projects/backup-check/src/main.c",
                       "page_of, 0, 3, 0);", timeout=20)
    check("its main.c is the template's, with its name",
          "/* backup-check: sections and rows" in main_c and
          "pu_side(&ui, 8, 10, 160, page_of, 0, 3, 0);" in main_c and
          'win_create(600, 380, "backup-check")' in main_c)
    first = q.command("cat .code-projects", "backup-check", timeout=20)
    check("first among Code's projects", path and path in first)
    end = time.time() + 20
    ps = ""
    while time.time() < end and "gcode" not in ps:
        ps = q.command("ps", "thread64", timeout=10)
        time.sleep(1)
    check("and Code is running", "gcode" in ps)


def rgb(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


if __name__ == "__main__":
    sys.exit(main())
