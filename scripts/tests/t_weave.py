#!/usr/bin/env python3
"""ThreadOS smoke test: Weave, ThreadOS's own desktop style.

'layout weave' (Settings, Desktop & Dock, "Interface style"; the design
in docs/WEAVE.md and docs/weave/canvas/, the plan in
docs/protonui-plan.md stage 3): opaque cloth with a woven grain instead
of glass, the window in front marked by one thread of colour, the loom
along the bottom in place of a dock or a taskbar, the design's own
icons and Inter for its letters. Read off the screen and done with the
mouse and the keys, against kernel/include/look.h's numbers, the
design's icon table (Icon.dc.html) and the grain's and the warp's
definitions computed here:

  the loom stands clear of the screen's edges, Night's cloth with the
  grain over it to the pixel; below it, the warp
  the warp: Night's ground and its 6x6 weave to the pixel where no
  thread crosses, and the person's thread (the accent) through its
  curve's middle
  the loom's Files icon: its dye with the icon's grain to the pixel, and
  the white of the design's own stroke across it
  a program started from the loom: its window carries the selvedge, the
  thread colour three pixels deep along its top, and under it a bar of
  Weave's height, Night's title cloth with the grain; its name in
  Inter's bold - which DejaVu does not read - and close the madder one
  of its buttons; under its program on the loom the long piece of thread
  a second program: the selvedge moves to it, and where the first
  window's selvedge was is the window's own colour
  a desktop's knot on the loom takes the screen there: no window, the
  knot filled
  the spool opens loom view: the ground, and the four threads of
  programs in their dyes; the keys walk along the first to Term, Enter
  starts it, and its thread on the loom says it is in front; Esc closes
  loom view
  a program asking for the look hears the Weave style, the grain's
  ink and Night's colours
  back to 'layout mac', the warp is gone

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, _kernel_define, LOOK, DOCK_APPS

L = LOOK
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(
    __file__))))


def warp_threads():
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "..", "kernel", "include",
                            "look.h")).read()
    m = re.search(r"#define WARP_THREADS((?:[^\n]*\\\n)*[^\n]*)", src)
    return [[int(v) for v in re.findall(r"-?\d+", row)]
            for row in re.findall(r"\{\s*(-?\d[^{}]*\{[^}]*\})\s*\}",
                                  m.group(1))]


def rgb(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


def mix(o, c, a):
    return tuple((p * (255 - a) + q * a + 127) // 255 for p, q in zip(o, c))


def grain(base, x, y):
    """kernel/gfx.c's grain over 'base' at screen pixel x, y (Night: white
    ink)"""
    ink = L["GRAIN_INK"] if y % 3 == 0 else \
        L["GRAIN_INK"] * 3 // 4 if x % 3 == 0 else 0
    return mix(rgb(base), (255, 255, 255), ink) if ink else rgb(base)


def warp_ground(x, y):
    a = L["WARP_ROW_INK"] if y % 6 == 3 else \
        L["WARP_COL_INK"] if x % 6 == 0 else 0
    g = rgb(L["NIGHT_GROUND"])
    return mix(g, (255, 255, 255), a) if a else g


def bezier(p, t):
    u = 1 - t
    return u * u * u * p[0] + 3 * u * u * t * p[1] + 3 * u * t * t * p[2] + \
        t * t * t * p[3]


def clear_patch(W, H, pw, ph):
    """A pw x ph patch of the warp's right half that no thread's glow
    comes near, worked out from the threads' curves in look.h: where the
    ground and the weave alone are to be expected"""
    pts = []
    for th in warp_threads():
        p = th[5:]
        x = [p[2 * i] * W * 16 // 1000 + 8 for i in range(6)]
        y = [p[2 * i + 1] * H * 16 // 1000 + 8 for i in range(6)]
        reach = max(th[1], th[3]) / 16.0 + 6
        for cx, cy in (((x[0], x[1], x[2], x[3]), (y[0], y[1], y[2], y[3])),
                       ((x[3], 2 * x[3] - x[2], x[4], x[5]),
                        (y[3], 2 * y[3] - y[2], y[4], y[5]))):
            for i in range(401):
                pts.append((bezier(cx, i / 400.0) / 16.0,
                            bezier(cy, i / 400.0) / 16.0, reach))
    for y0 in range(60, H // 2, 4):
        for x0 in range(W - pw - 20, W // 2 + 40, -10):
            if all(px < x0 - r or px > x0 + pw + r or
                   py < y0 - r or py > y0 + ph + r for px, py, r in pts):
                return x0, y0
    return None


def selvedges(at, W, H, accent):
    """Runs of the thread colour 100 pixels and longer, three rows deep:
    (x0, x1, y) of each selvedge on the screen"""
    found = []
    for y in range(0, H - 3):
        x = 0
        while x < W:
            if at(x, y) == accent:
                x0 = x
                while x < W and at(x, y) == accent:
                    x += 1
                if x - x0 >= 100 and all(at(x0 + 5, y + d) == accent
                                         for d in (1, 2)) and \
                        at(x0 + 5, y + 3) != accent and \
                        (y == 0 or at(x0 + 5, y - 1) != accent):
                    found.append((x0, x, y))
            else:
                x += 1
    return found


def patch_is(at, x0, y0, w, h, want):
    wrong = [(x, y) for y in range(y0, y0 + h) for x in range(x0, x0 + w)
             if at(x, y) != want(x, y)]
    return wrong


def design_icon(name):
    """The design's own definition of an icon (Icon.dc.html): its dye and
    its stroked paths"""
    src = open(os.path.join(ROOT, "docs", "weave", "canvas",
                            "Icon.dc.html")).read()
    m = re.search(r"^\s*%s: \['(#[0-9A-Fa-f]{6})', '[^']*', \[(.*)\]\],?\s*$"
                  % name, src, re.M)
    return int(m.group(1)[1:], 16), re.findall(r"S\('([^']*)'\)",
                                               m.group(2))


def loom_geom(W, H, n):
    """The loom laid out from look.h's numbers, as Weave lays it: its
    top, the spool, the programs' icons (n of them) and the knots"""
    top = H - L["LOOM_GAP"] - L["LOOM_H"]
    left = L["LOOM_SIDE"] + 1 + L["LOOM_PAD"]
    right = W - L["LOOM_SIDE"] - 1 - L["LOOM_PAD"]
    clock_x = right - L["LOOM_CLOCK_W"]
    # (QEMU's machine has no battery)
    tray_x = clock_x - L["LOOM_SPACE"] - 4 - 2 * L["LOOM_GLYPH"] - \
        L["LOOM_TRAY_GAP"]
    d, kg = L["LOOM_KNOT_D"], L["LOOM_KNOT_GAP"]
    knot0 = tray_x - L["LOOM_SPACE"] - (4 * d + 3 * kg + 8) + 4
    find_x = left + L["LOOM_SPOOL"] + L["LOOM_SPACE"]
    room = knot0 - 4 - L["LOOM_SPACE"] - find_x - L["LOOM_SPACE"]
    need = n * (L["LOOM_ICON"] + L["LOOM_ICON_GAP"]) - L["LOOM_ICON_GAP"] + 8
    fw = L["LOOM_FIND_W"] if room - L["LOOM_FIND_W"] >= need else \
        L["LOOM_FIND_H"]
    x0 = find_x + fw + L["LOOM_SPACE"]
    x1 = knot0 - 4 - L["LOOM_SPACE"]
    s, slot = L["LOOM_ICON"], L["LOOM_ICON"] + L["LOOM_ICON_GAP"]
    if n and n * slot - L["LOOM_ICON_GAP"] > x1 - x0:
        slot = (x1 - x0 + L["LOOM_ICON_GAP"]) // n
        s = max(slot - L["LOOM_ICON_GAP"], L["LOOM_ICON_MIN"])
    block = n * slot - (slot - s) if n else 0
    px = max(x0 + (x1 - x0 - block) // 2, x0)
    ky = top + L["LOOM_ICON_Y"] + (L["LOOM_ICON"] - s) // 2
    return dict(top=top, left=left,
                icons=[(px + k * slot, ky, s) for k in range(n)],
                knots=[knot0 + i * (d + kg) for i in range(4)],
                line=top + L["LOOM_LINE_Y"])


def icon_tile(dye, s, i, j):
    """Pixel i, j of an icon's tile s across, away from its mark (the
    design's grain: a dark warp every third column, a light weft every
    third row from the foot over it, a dark last row)"""
    c = rgb(dye)
    if i % 3 == 0:
        c = mix(c, (0, 0, 0), L["WEAVE_WARP_INK"])
    if (s - 1 - j) % 3 == 0:
        c = mix(c, (255, 255, 255), L["WEAVE_WEFT_INK"])
    if j >= s - max(1, (s + 16) // 32):
        c = mix(c, (0, 0, 0), L["WEAVE_EDGE_INK"])
    return c


def main():
    print("--- Weave, ThreadOS's own style ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    accent = rgb(L["NIGHT_ACCENT"])
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop())
            W, H = q.size()
            q.click_desktop()
            check("'layout weave'",
                  "saved" in q.sudo("set layout weave", "saved"))
            end = time.time() + 10
            while time.time() < end and "the Weave style" not in q.serial():
                time.sleep(0.3)
            check("the desktop takes it", "the Weave style" in q.serial())
            q.mouse_goto(W // 2, 40)
            time.sleep(2)

            n = len(DOCK_APPS)
            lg = loom_geom(W, H, n)
            top = lg["top"]
            _, _, at = q.screen_raw()
            # a strip of the loom above the icons: cloth and grain to the
            # pixel
            wrong = patch_is(at, W // 2 - 20, top + 1, 40, 4,
                             lambda x, y: grain(L["NIGHT_DOCK_TINT"], x, y))
            check("the loom: Night's cloth with the grain over it (%d of "
                  "160 pixels wrong%s)" % (len(wrong), ", first %s: %s" % (
                      wrong[0], at(*wrong[0])) if wrong else ""), not wrong)
            # beside it and below it the warp, in the loom's shadow:
            # darker than the cloth, and no cloth
            cloth = rgb(L["NIGHT_DOCK_TINT"])
            outside = [(2, top + 10), (W // 2, H - 2), (W - 3, top + 30)]
            check("clear of the screen's edges: no cloth beside it and "
                  "below it (%s)" % [at(x, y) for x, y in outside],
                  all(at(x, y) != grain(L["NIGHT_DOCK_TINT"], x, y) and
                      sum(at(x, y)) < sum(cloth) for x, y in outside))
            # (the desktop's icons stand at the top left, the pointer at
            # the top in the middle)
            patch = clear_patch(W, H, 60, 40)
            wrong = patch_is(at, patch[0], patch[1], 60, 40, warp_ground) \
                if patch else [None]
            check("the warp: Night's ground and its 6x6 weave, to the "
                  "pixel, where no thread passes (%s: %d of 2400 wrong)" %
                  (patch, len(wrong)), not wrong)
            th = [t for t in warp_threads() if t[0] == 6][0]
            px, py = th[5 + 6] * W * 16 // 1000 + 8, \
                th[5 + 7] * H * 16 // 1000 + 8
            c = at(px // 16, py // 16)
            g = warp_ground(px // 16, py // 16)
            dist = lambda a, b: sum(abs(p - q) for p, q in zip(a, b))
            check("and the person's own thread through its curve's middle "
                  "(%s, nearer the accent than the ground)" % (c,),
                  dist(c, accent) < dist(c, g))

            # the loom's Files icon, against the design's definition
            fx, fy, fs = lg["icons"][DOCK_APPS.index("gfiles")]
            dye, paths = design_icon("files")
            wrong = patch_is(at, fx + fs // 2 - 6, fy + 2, 12, 4,
                             lambda x, y: icon_tile(dye, fs, x - fx, y - fy))
            check("the Files icon: the design's dye %06x with the icon's "
                  "grain, to the pixel (%d of 48 wrong%s)"
                  % (dye, len(wrong), ", first %s" % (at(*wrong[0]),)
                     if wrong else ""), not wrong)
            hl = [re.match(r"M([\d.]+) ([\d.]+)h([\d.]+)$", p) for p in paths]
            hl = [m for m in hl if m]
            if hl:
                x0d, yd, ld = (float(v) for v in hl[0].groups())
                row = int(yd * fs / 48)
                xs = range(fx + int(x0d * fs / 48) + 2,
                           fx + int((x0d + ld) * fs / 48) - 1)
                white = sum(at(x, fy + row) == (255, 255, 255) for x in xs)
            check("and its stroke (the design's %s) white across it (%d of "
                  "%d pixels)" % (hl[0].group(0) if hl else "?",
                                  white if hl else 0, len(xs) if hl else 0),
                  hl and white == len(xs))

            # Files, from the loom
            q.click(fx + fs // 2, fy + fs // 2)
            q.mouse_goto(W // 2, 20)
            sv = []
            end = time.time() + 20
            while time.time() < end and len(sv) != 1:
                time.sleep(0.7)
                _, _, at = q.screen_raw()
                sv = selvedges(at, W, H, accent)
            check("a program from the loom: its window carries the "
                  "selvedge (%s)" % sv, len(sv) == 1)
            if len(sv) == 1:
                x0, x1, y0 = sv[0]
                mx = (x0 + x1) // 2
                bar = y0 + L["SELVEDGE_H"]
                wrong = patch_is(at, mx, bar + 1, 30, L["WEAVE_BAR_H"] - 2,
                                 lambda x, y: grain(L["NIGHT_TITLE_BG"],
                                                    x, y))
                check("its title bar is Night's title cloth with the grain "
                      "(%d wrong)" % len(wrong), not wrong)
                check("Weave's bar, %d pixels under the selvedge, then the "
                      "line (%s)" % (L["WEAVE_BAR_H"],
                                     at(mx, y0 + L["WEAVE_TITLE_H"])),
                      at(mx, y0 + L["WEAVE_TITLE_H"]) ==
                      rgb(L["NIGHT_WIN_LINE"]) and
                      at(mx, y0 + L["WEAVE_TITLE_H"] - 1) ==
                      grain(L["NIGHT_TITLE_BG"], mx,
                            y0 + L["WEAVE_TITLE_H"] - 1))
                # the window's left edge is the selvedge's start less the
                # corner; the name after the icon and its gap
                wx = x0 - L["WEAVE_WIN_R"]
                pen = wx + L["WEAVE_BAR_LEFT"] + L["WEAVE_TITLE_ICON"] + \
                    L["WEAVE_TITLE_GAP"]
                base = ui_font_baseline_in(L["WEAVE_TITLE_PX"], bar,
                                           L["WEAVE_BAR_H"])
                under = lambda x, y: grain(L["NIGHT_TITLE_BG"], x, y)
                inter = q.read_ui(pen, base, width=60, px=L["WEAVE_TITLE_PX"],
                                  face="interb", under=under,
                                  fg=rgb(L["NIGHT_TITLE_TEXT"]),
                                  screen=(W, H, at))
                dejavu = q.read_ui(pen, base, width=60,
                                   px=L["WEAVE_TITLE_PX"], face="bold",
                                   under=under, fg=rgb(L["NIGHT_TITLE_TEXT"]),
                                   screen=(W, H, at))
                check("its name in Inter's bold ('%s'; as DejaVu: '%s')"
                      % (inter, dejavu),
                      inter.startswith("Files") and
                      not dejavu.startswith("Files"))
                # close, at the capsule's right end: its sign in the
                # light madder, its ground madder; minimise's ground not
                cy = y0 + L["SELVEDGE_H"] + \
                    (L["WEAVE_BAR_H"] - L["WEAVE_BTN"]) // 2 + L["WEAVE_BTN"] // 2
                wr = x1 + L["WEAVE_WIN_R"]
                cx = wr - L["WEAVE_CAP_RIGHT"] - L["WEAVE_CAP_PAD"] - \
                    L["WEAVE_BTN"] // 2
                mxb = cx - 2 * (L["WEAVE_BTN"] + L["WEAVE_BTN_GAP"])
                sign, ground, other = at(cx, cy), at(cx - 9, cy), \
                    at(mxb - 9, cy)
                check("close: its sign the light madder %06x (%s), its "
                      "ground madder (%s) where minimise's is not (%s)"
                      % (L["WEAVE_CLOSE_INK"], sign, ground, other),
                      dist(sign, rgb(L["WEAVE_CLOSE_INK"])) < 90 and
                      ground[0] - ground[1] > 12 and
                      abs(other[0] - other[1]) < 8)
                # the long piece of thread under the program in front
                run = sum(at(x, lg["line"] + 1) == accent
                          for x in range(fx - 6, fx + fs + 6))
                check("on the loom, under the Files icon, the long piece "
                      "of thread (%d pixels)" % run,
                      run >= L["LOOM_THREAD_FOCUS"] - 2)

                # Settings in front of it: the selvedge moves
                sx, sy, ss = lg["icons"][DOCK_APPS.index("gsettings")]
                q.click(sx + ss // 2, sy + ss // 2)
                q.mouse_goto(W // 2, 20)
                sv2 = []
                end = time.time() + 20
                while time.time() < end and (len(sv2) != 1 or sv2 == sv):
                    time.sleep(0.7)
                    _, _, at = q.screen_raw()
                    sv2 = selvedges(at, W, H, accent)
                check("a second program: the selvedge goes to its window "
                      "(%s)" % sv2, len(sv2) == 1 and sv2 != sv)
                if len(sv2) == 1 and sv2 != sv:
                    nx0, nx1, ny0 = sv2[0]
                    # the second window, by its title bar, down and out of
                    # the way: the cascade puts it over nearly all of the
                    # first one's top
                    q.mouse_goto((nx0 + nx1) // 2, ny0 + L["WEAVE_TITLE_H"] // 2)
                    q.mouse_button(True)
                    q.mouse_goto((nx0 + nx1) // 2,
                                 ny0 + L["WEAVE_TITLE_H"] // 2 + 160)
                    q.mouse_button(False)
                    q.mouse_goto(W // 2, H // 2)
                    sv3 = []
                    end = time.time() + 10
                    while time.time() < end:
                        time.sleep(0.5)
                        _, _, at = q.screen_raw()
                        sv3 = selvedges(at, W, H, accent)
                        if len(sv3) == 1 and sv3[0][2] > ny0 + 100:
                            break
                    check("the second window moves down, its selvedge with "
                          "it (%s)" % sv3, len(sv3) == 1 and
                          sv3[0][2] > ny0 + 100)
                    # where the first window's selvedge was, below its
                    # outline: the window's own colour; its bar the cloth
                    # of one without focus
                    free = list(range(x0 + 12, x1 - 12, 3))
                    wrong = [(x, y) for x in free
                             for y in range(y0 + 1, y0 + L["SELVEDGE_H"])
                             if at(x, y) != rgb(L["NIGHT_WIN_BG"])]
                    wrong += [(x, bar + 2) for x in free
                              if at(x, bar + 2) !=
                              grain(L["NIGHT_TITLE_OFF"], x, bar + 2)]
                    check("and where the first window's selvedge was, its "
                          "own colour, over the bar of one without focus "
                          "(%d pixels looked at, %d wrong)"
                          % (3 * len(free), len(wrong)), free and not wrong)

            # a desktop's knot
            kx = lg["knots"][1] + L["LOOM_KNOT_D"] // 2
            ky = top + L["LOOM_H"] // 2
            q.click(kx, ky)
            q.mouse_goto(W // 2, 20)
            ok = False
            end = time.time() + 10
            while time.time() < end and not ok:
                time.sleep(0.7)
                _, _, at = q.screen_raw()
                ok = not selvedges(at, W, H, accent) and at(kx, ky) == accent
            check("a desktop's knot takes the screen there: no window, its "
                  "knot filled", ok)

            # loom view, from the spool
            ground = (8, 9, 13)
            q.click(lg["left"] + L["LOOM_SPOOL"] // 2, top + L["LOOM_H"] // 2)
            q.mouse_goto(W - 5, H // 2)
            seen = False
            end = time.time() + 10
            while time.time() < end and not seen:
                time.sleep(0.5)
                _, _, at = q.screen_raw()
                seen = at(5, 5) == ground and at(W // 2, 2) == ground
            check("the spool opens loom view: its ground", seen)
            dyes = [L["DYE_INDIGO_N"], L["DYE_WOAD_N"], L["DYE_COCHINEAL_N"],
                    L["DYE_SAFFRON_N"]]
            want = [mix(ground, rgb(d_), 217) for d_ in dyes]
            found = []
            for y in range(0, top):
                c = at(W - 60, y)
                if c in want and (not found or found[-1][0] != c or
                                  found[-1][2] != y - 1):
                    found.append([c, y, y])
                elif found and c == found[-1][0] and found[-1][2] == y - 1:
                    found[-1][2] = y
            threads = [want.index(f[0]) for f in found if f[2] - f[1] == 2]
            check("four threads of programs, each three rows of its dye: "
                  "Indigo, Woad, Cochineal, Saffron (%s)" % threads,
                  threads == [0, 1, 2, 3])
            # the keys along the first thread to Term, and Enter
            src = open(os.path.join(ROOT, "kernel", "desktop.c")).read()
            work = re.findall(r'"(\w+)"', re.search(
                r'\{ "Work",[^{]*\{([^}]*)\}', src).group(1))
            # the first press puts the keys on the thread's first program
            for _ in range(work.index("gterm") + 1):
                q.sendkey("right")
            q.sendkey("ret")
            tx, ty_, ts = lg["icons"][DOCK_APPS.index("gterm")]
            ok = False
            end = time.time() + 20
            while time.time() < end and not ok:
                time.sleep(0.7)
                _, _, at = q.screen_raw()
                run = sum(at(x, lg["line"] + 1) == accent
                          for x in range(tx - 6, tx + ts + 6))
                ok = at(5, 5) != ground and run >= L["LOOM_THREAD_FOCUS"] - 2
            check("arrows along the first thread to Term and Enter: loom "
                  "view goes and Term is in front, its long thread on the "
                  "loom", ok)
            q.click(lg["left"] + L["LOOM_SPOOL"] // 2, top + L["LOOM_H"] // 2)
            time.sleep(1.5)
            _, _, at = q.screen_raw()
            opened = at(5, 5) == ground
            q.sendkey("esc")
            time.sleep(1.5)
            _, _, at = q.screen_raw()
            check("Esc closes loom view again", opened and at(5, 5) != ground)

            # to an empty desktop, so that what is typed next goes to the
            # system's shell and not into Term
            q.click(lg["knots"][2] + L["LOOM_KNOT_D"] // 2, ky)
            q.mouse_goto(W // 2, 20)
            time.sleep(1)

            out = q.command("run pulook", "[pulook] done", timeout=60)
            m = re.search(r"LOOK v2((?: [0-9a-f]{8}){64})", out)
            words = [int(w, 16) for w in m.group(1).split()] if m else []
            check("a program hears the Weave style, the grain's ink and "
                  "Night's window colour (%s)" % words[:22],
                  words and words[11] == _kernel_define(
                      "kernel/include/desktop.h", "UI_STYLE_WEAVE") and
                  words[19] == L["GRAIN_INK"] and
                  words[1] == L["NIGHT_WIN_BG"] and
                  words[7] == L["NIGHT_ACCENT"])

            q.sudo("set layout mac", "saved")
            end = time.time() + 10
            while time.time() < end and "the Mac style" not in q.serial():
                time.sleep(0.3)
            time.sleep(1.5)
            _, _, at = q.screen_raw()
            same = 2400 - len(patch_is(at, patch[0], patch[1], 60, 40,
                                       warp_ground)) if patch else 2400
            check("back to 'layout mac': no loom, the warp gone (%d of 2400 "
                  "pixels still the warp's)" % same,
                  "the Mac style" in q.serial() and same < 1200)
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
