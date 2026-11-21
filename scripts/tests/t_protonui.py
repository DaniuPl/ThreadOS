#!/usr/bin/env python3
"""ThreadOS smoke test: ProtonUI v2, the toolkit programs draw with.

ProtonUI v2 (docs/protonui-plan.md, the design in docs/WEAVE.md) is
built in stages; this group grows with them.

Stage 1, the look: SYS64_UI_LOOK hands a program the theme as numbers.
It was eleven words; v2 asks for twenty-two - the desktop's style,
Weave's six dyes in the theme, the person's thread colour, the grain,
a control's corners, the density. A program built before v2 asks with
no length and must get exactly the eleven it has room for:
libc/tests/pulook fills its arrays with a guard word, asks the old way,
for 22, for 64, and for the dark and the light theme, and prints them.
The guard must stand right after the eleventh word, and after the
twenty-second when more were asked for; the dyes must be
kernel/include/look.h's, Night's in the dark theme and Linen's in the
light one; the thread is the person's accent. The phone answers the
same call (t_arm64win runs the same program there).

Stage 2, Weave's four marks: user/pumarks fills a window with a colour
nothing else has and draws the grain, a stitch across and one down,
three knots and a thread on it. The window is found on the screen by
that colour, and the marks are compared with what they are by
definition, computed here: the grain pixel for pixel (the weft row at
full ink, the warp column at three quarters, mixed in whole numbers -
and through a clip, still counted from the window's corner), the
stitch's dashes and gaps, each knot's centre and the ring around it
(hollow: an empty middle; ringed: a dot in it and room between; filled:
all of it), and the thread through the points of its Bezier curve, with
the background a little way off it.

Stage 4, the controls: user/pudemo puts every control of ProtonUI v2
in a window with pu_start - buttons of the four kinds, two switches,
segments, knots, two boxes, a field, a slider, a list and both kinds of
progress - and says on the serial port what it was told. Under Weave
each is read off the screen against the design's colours (user/ugui.h's
numbers, look.h's): the one thing to do in the accent, another thing a
raised surface with its line, a quiet one only its words, the
destroying one on madder; a switch on (the accent, the white knob at
its right) and off (the grey knob at its left); the chosen segment
raised with the thread along its foot; the chosen knot filled and the
others hollow; the boxes; the field's stitch, dashed at rest and the
thread with the keys; progress spun as far as its value with its knot;
the chosen row. Then clicks move them and the program prints the new
values; Tab gives the keys to the first control and the thread wraps
round it, Space presses it, Tab again moves the thread on; typing
reaches the field. Under the Mac style the same window is drawn the
old way: another thing is a row's grey, not Weave's raised surface.

Run on its own or through scripts/run_tests.py.
"""

import math
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, _kernel_define, LOOK

GUARD = 0xA5A5A5A5
UI_LOOK_N = _kernel_define("kernel/include/desktop.h", "UI_LOOK_N")
UI_LOOK_MAX = _kernel_define("kernel/include/desktop.h", "UI_LOOK_MAX")
STYLE_BAR_DOCK = _kernel_define("kernel/include/desktop.h",
                                 "UI_STYLE_BAR_DOCK")
STYLE_PHONE = _kernel_define("kernel/include/desktop.h", "UI_STYLE_PHONE")
DYES = ("INDIGO", "MADDER", "SAFFRON", "WELD", "WOAD", "COCHINEAL")


def looks(out):
    """pulook's lines: {"v1": [64 words], ...}"""
    got = {}
    for m in re.finditer(r"LOOK (\w+)((?: [0-9a-f]{8}){64})", out):
        got[m.group(1)] = [int(w, 16) for w in m.group(2).split()]
    return got


def check_look(out, where, style, density):
    """Everything pulook printed, against look.h - on the PC or the
    phone ('where'), whose style and density are given"""
    got = looks(out)
    check("%s: pulook printed all five answers (%s)"
          % (where, sorted(got)), len(got) == 5)
    if len(got) != 5:
        return
    v1, v2, big = got["v1"], got["v2"], got["big"]
    check("%s: the old call fills eleven words and not one past them"
          % where, all(w != GUARD for w in v1[:UI_LOOK_N]) and
          all(w == GUARD for w in v1[UI_LOOK_N:]))
    check("%s: asked for %d, the first eleven are the old call's"
          % (where, UI_LOOK_MAX), v2[:UI_LOOK_N] == v1[:UI_LOOK_N])
    check("%s: and %d words come, the guard after them untouched"
          % (where, UI_LOOK_MAX),
          all(w == GUARD for w in v2[UI_LOOK_MAX:]) and
          GUARD not in v2[:UI_LOOK_MAX])
    check("%s: asked for 64, still %d and nothing past them"
          % (where, UI_LOOK_MAX), big == v2)
    for theme, suffix, dark in (("night", "N", 1), ("linen", "L", 0)):
        w = got[theme]
        want = [LOOK["DYE_%s_%s" % (d, suffix)] for d in DYES]
        check("%s: the %s theme says it is %s, and its six dyes are "
              "look.h's (%s)" % (where, theme, "dark" if dark else "light",
                                 " ".join("%06x" % x for x in w[12:18])),
              w[0] == dark and w[12:18] == want)
        check("%s: %s: the style %d, the thread is the accent (%06x), "
              "no grain outside Weave, corners %d, density %d"
              % (where, theme, w[11], w[18], w[20], w[21]),
              w[11] == style and w[18] == w[7] and w[19] == 0 and
              w[20] == LOOK["CONTROL_R"] and w[21] == density)
    check("%s: done" % where, "[pulook] done" in out)


def marks_define(name):
    return _kernel_define("user/pumarks.c", name)


M = {n: marks_define(n) for n in (
    "W", "H", "INK", "GRAIN_X", "GRAIN_Y", "GRAIN_W", "GRAIN_H", "CLIP_X",
    "CLIP_Y", "CLIP_W", "CLIP_H", "STITCH_X", "STITCH_Y", "STITCH_LEN",
    "VSTITCH_X", "VSTITCH_Y", "KNOT_X", "KNOT_Y", "KNOT_D", "KNOT_STEP",
    "THREAD_W", "T0X", "T0Y", "T1X", "T1Y", "T2X", "T2Y", "T3X", "T3Y")}
BG = (0x10, 0x20, 0x30)
INK_RGB = (0xFF, 0xFF, 0xFF)
STITCH_RGB = (0xF0, 0xD0, 0x20)
KNOT_RGB = (0x46, 0xC2, 0x7A)
THREAD_RGB = (0xD1, 0x52, 0xA6)


def find_window(at, sw, sh):
    """The content's corner: the first row with a long run of the colour
    pumarks' window is filled with (a stray pixel of it elsewhere - a
    letter's edge - is no window), and how wide and tall that colour
    reaches from the run's start"""
    for y in range(0, sh):
        x = 0
        while x < sw:
            if at(x, y) != BG:
                x += 4
                continue
            x0 = x
            while x0 > 0 and at(x0 - 1, y) == BG:
                x0 -= 1
            x1 = x
            while x1 < sw and at(x1, y) == BG:
                x1 += 1
            if x1 - x0 >= 100:
                y1 = y
                while y1 < sh and at(x0, y1) == BG:
                    y1 += 1
                return x0, y, x1 - x0, y1 - y
            x = x1 + 4
    return None


def grain_at(i, j):
    """The grain's definition (kernel/gfx.c): ink on the weft row, three
    quarters of it on the warp column alone, mixed in whole numbers"""
    ink = M["INK"] if j % 3 == 0 else M["INK"] * 3 // 4 if i % 3 == 0 else 0
    return tuple((b * (255 - ink) + c * ink + 127) // 255
                 for b, c in zip(BG, INK_RGB))


def close(a, b, tol=10):
    return all(abs(p - q) <= tol for p, q in zip(a, b))


def bezier(t):
    p = [(M["T%dX" % i], M["T%dY" % i]) for i in range(4)]
    u = 1 - t
    return tuple(u ** 3 * p[0][k] + 3 * u * u * t * p[1][k] +
                 3 * u * t * t * p[2][k] + t ** 3 * p[3][k] + 0.5
                 for k in (0, 1))


def check_marks(at, ox, oy):
    def px(x, y):
        return at(ox + x, oy + y)
    wrong = [(i, j) for j in range(M["GRAIN_Y"], M["GRAIN_Y"] + M["GRAIN_H"])
             for i in range(M["GRAIN_X"], M["GRAIN_X"] + M["GRAIN_W"])
             if px(i, j) != grain_at(i, j)]
    check("the grain is the 3x3 tile to the pixel, %d pixels (%d wrong%s)"
          % (M["GRAIN_W"] * M["GRAIN_H"], len(wrong),
             ", first at %s: %s, not %s" % (wrong[0], px(*wrong[0]),
                                           grain_at(*wrong[0]))
             if wrong else ""), not wrong)
    wrong = [(i, j) for j in range(M["CLIP_Y"], M["CLIP_Y"] + M["CLIP_H"])
             for i in range(M["CLIP_X"], M["CLIP_X"] + M["CLIP_W"])
             if px(i, j) != grain_at(i, j)]
    check("through a clip it is still counted from the window's corner "
          "(%d wrong)" % len(wrong), not wrong)
    x, y, n = M["STITCH_X"], M["STITCH_Y"], M["STITCH_LEN"]
    want = [STITCH_RGB if (k // 4) % 2 == 0 else BG for k in range(n)]
    across = [px(x + k, y + d) for d in (0, 1) for k in range(n)]
    check("a stitch across: dashes of 4 and gaps of 4, two pixels thick",
          across == want * 2 and px(x, y + 2) == BG and px(x, y - 1) == BG)
    x, y = M["VSTITCH_X"], M["VSTITCH_Y"]
    down = [px(x + d, y + k) for d in (0, 1) for k in range(n)]
    check("and one down", down == want * 2 and px(x + 2, y) == BG)
    d, r = M["KNOT_D"], M["KNOT_D"] // 2
    for state, name in enumerate(("hollow", "ringed", "filled")):
        kx, ky = M["KNOT_X"] + state * M["KNOT_STEP"], M["KNOT_Y"]
        centre, edge = px(kx + r, ky + r), px(kx + r, ky + 1)
        between = px(kx + r, ky + r - r * 2 // 3)
        outside = px(kx + r, ky - 2)
        want_centre = BG if state == 0 else KNOT_RGB
        want_between = KNOT_RGB if state == 2 else BG
        check("a %s knot: its edge, its middle %s, between them %s "
              "(%s, %s, %s)" % (name, "empty" if state == 0 else "full",
                                "full" if state == 2 else "empty",
                                edge, centre, between),
              close(edge, KNOT_RGB) and close(centre, want_centre) and
              close(between, want_between) and outside == BG)
    on, off = [], []
    for t in (0.2, 0.35, 0.5, 0.65, 0.8):
        bx, by = bezier(t)
        on.append(px(int(bx), int(by)))
        far = (int(bx), int(by) + 7)
        near = min(math.hypot(far[0] + 0.5 - bezier(s / 400)[0],
                              far[1] + 0.5 - bezier(s / 400)[1])
                   for s in range(401))
        if near > 5:
            off.append(px(*far))
    check("the thread runs through its Bezier curve's points (%s)" % on,
          all(close(c, THREAD_RGB, 24) for c in on))
    check("and leaves the cloth a few pixels off it (%d looked at)"
          % len(off), off and all(c == BG for c in off))


def marks():
    """Stage 2 on the desktop"""
    slog = tempfile.mktemp(suffix=".log")
    q = Qemu(KERNEL, INITRD, serial=slog)
    try:
        check("the desktop came up", q.wait_desktop())
        q.type_line("run pumarks")
        q.wait_serial("[pumarks] drawn", timeout=60)
        # the pointer starts in the middle of the screen, where the
        # window opens: out of the way of what is read
        sw, sh = q.size()
        q.mouse_goto(sw - 10, sh // 2)
        found, at = None, None
        end = time.time() + 30
        while time.time() < end:
            sw, sh, at = q.screen_raw()
            found = find_window(at, sw, sh)
            if found and found[2] == M["W"]:
                break
            time.sleep(1)
        check("pumarks' window is on the screen, %dx%d (%s)"
              % (M["W"], M["H"], found), found and found[2] == M["W"])
        if found and found[2] == M["W"]:
            check_marks(at, found[0], found[1])
    finally:
        q.close()
        if os.path.exists(slog):
            os.unlink(slog)


def demo_define(name):
    return _kernel_define("user/pudemo.c", name)


D = {n: demo_define(n) for n in (
    "W", "H", "BTN_Y", "BTN_H", "B0_X", "B0_W", "B1_X", "B1_W", "B2_X",
    "B2_W", "B3_X", "B3_W", "SW_X", "SW_Y", "SW_W", "SW2_Y", "SEG_X",
    "SEG_Y", "SEG_W", "SEG_H", "KN_X", "KN_Y", "CK_X", "CK_Y", "CK2_Y",
    "FLD_X", "FLD_Y", "FLD_W", "LST_X", "LST_Y", "LST_W", "PR_X", "PR_Y",
    "PR_W", "PR_VALUE")}


def ugui_colours():
    """user/ugui.h's colours for Weave's controls (PU_N_*: Night)"""
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "..", "user", "ugui.h")).read()
    return {m.group(1): int(m.group(2), 16) for m in re.finditer(
        r"#define\s+(PU_N_\w+)\s+0x([0-9A-Fa-f]+)", src)}


def ugui_define(name):
    return _kernel_define("user/ugui.h", name)


def rgb(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


def mix(over, under, a):
    return tuple((o * a + u * (255 - a) + 127) // 255
                 for o, u in zip(rgb(over), rgb(under)))


def wait_said(q, text, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        if text in q.serial():
            return True
        time.sleep(0.3)
    return False


def selvedge(at, sw, sh, accent):
    """The window in front's selvedge: (x0, y0) of the run of the thread
    colour, three rows deep, that starts it"""
    for y in range(0, sh - 3):
        x = 0
        while x < sw:
            if at(x, y) != accent:
                x += 1
                continue
            x0 = x
            while x < sw and at(x, y) == accent:
                x += 1
            if x - x0 >= 100 and at(x0 + 5, y + 1) == accent and \
                    at(x0 + 5, y + 2) == accent and \
                    at(x0 + 5, y + 3) != accent:
                return x0, y
    return None


def controls():
    """Stage 4 on the desktop, under Weave and then the Mac style"""
    C = ugui_colours()
    accent, bg = LOOK["NIGHT_ACCENT"], LOOK["NIGHT_WIN_BG"]
    danger = LOOK["NIGHT_APP_DANGER"]
    slog = tempfile.mktemp(suffix=".log")
    q = Qemu(KERNEL, INITRD, serial=slog)
    try:
        check("the desktop came up", q.wait_desktop())
        sw, sh = q.size()
        q.click_desktop()
        q.sudo("set layout weave", "saved")
        check("Weave", wait_said(q, "the Weave style"))
        q.click_desktop()
        # 'runw': the shell does not wait for it, and takes the commands
        # that come later
        q.type_line("runw pudemo")
        check("pudemo's window is drawn", wait_said(q, "[pudemo] drawn", 60))
        q.mouse_goto(sw - 10, sh // 2)
        found = None
        end = time.time() + 20
        while time.time() < end and not found:
            time.sleep(0.5)
            _, _, at = q.screen_raw()
            found = selvedge(at, sw, sh, rgb(accent))
        check("its window, found by its selvedge (%s)" % (found,), found)
        if not found:
            return
        wx = found[0] - LOOK["WEAVE_WIN_R"]
        ox, oy = wx + 1, found[1] + LOOK["WEAVE_TITLE_H"] + 1

        def px(x, y):
            return at(ox + x, oy + y)

        def near(a, b, tol=6):
            return all(abs(p - q_) <= tol for p, q_ in zip(a, b))

        def shot():
            nonlocal at
            time.sleep(1)
            _, _, at = q.screen_raw()

        def click(x, y):
            """a click in the window, and the pointer out of the way of
            what is read after it"""
            q.click(ox + x, oy + y)
            q.mouse_goto(sw - 10, sh // 2)

        my = D["BTN_Y"] + D["BTN_H"] // 2
        got = [px(D[k] + 5, my) for k in ("B0_X", "B1_X", "B2_X", "B3_X")]
        check("buttons: the one thing to do in the accent, another raised "
              "with its line, a quiet one only its words, the destroying "
              "one on madder (%s)" % got,
              got[0] == rgb(accent) and got[1] == rgb(C["PU_N_RAISED"]) and
              near(px(D["B1_X"], my), rgb(C["PU_N_RAISED_LINE"]), 12) and
              got[2] == rgb(bg) and
              got[3] == mix(danger, bg, ugui_define("PU_DANGER_A")))

        sx = D["SW_X"] + D["SW_W"] - ugui_define("PU_SWITCH_W")
        s1, s2 = D["SW_Y"] + 2, D["SW2_Y"] + 2
        on = (px(sx + 6, s1 + 11), px(sx + 29, s1 + 11))
        off = (px(sx + 11, s2 + 11), px(sx + 30, s2 + 11))
        check("a switch on: the accent, the white knob at its right; off: "
              "the grey knob at its left on a quiet pill (%s %s)" % (on, off),
              on == (rgb(accent), (255, 255, 255)) and
              off == (rgb(C["PU_N_KNOB_OFF"]), rgb(C["PU_N_SEG_ON"])))

        seg = lambda i: D["SEG_X"] + 3 + (D["SEG_W"] - 6) * i // 3
        foot = D["SEG_Y"] + D["SEG_H"] - 5
        chosen = (px(seg(0) + 4, D["SEG_Y"] + 8),
                  px((seg(0) + seg(1)) // 2, foot))
        other = px(seg(2) + 4, D["SEG_Y"] + 8)
        check("segments: the chosen one raised, the thread along its foot; "
              "another not raised (%s %s)" % (chosen, other),
              chosen == (rgb(C["PU_N_SEG_ON"]), rgb(accent)) and
              other == rgb(C["PU_N_FIELD"]))

        kc = lambda r: px(D["KN_X"] + 8, D["KN_Y"] + r * 24 + 12)
        check("knots: the chosen one filled in the accent, the others "
              "hollow (%s)" % [kc(r) for r in range(3)],
              kc(1) == rgb(accent) and kc(0) == rgb(bg) and kc(2) == rgb(bg))

        b1, b2 = D["CK_Y"] + 1, D["CK2_Y"] + 1
        check("boxes: ticked, the accent's; not, empty inside a grey ring "
              "(%s %s)" % (px(D["CK_X"] + 2, b1 + 2), px(D["CK_X"] + 8, b2 + 8)),
              px(D["CK_X"] + 2, b1 + 2) == rgb(accent) and
              px(D["CK_X"] + 8, b2 + 8) == rgb(bg) and
              near(px(D["CK_X"] + 8, b2), rgb(C["PU_N_KNOT"]), 30))

        fy = D["FLD_Y"] + ugui_define("UG_ROW") + 4 - 2
        row = [px(x, fy) for x in range(D["FLD_X"], D["FLD_X"] + D["FLD_W"])]
        st, fi = row.count(rgb(C["PU_N_STITCH"])), row.count(rgb(C["PU_N_FIELD"]))
        check("the field at rest: its stitch dashed under it (%d stitch, %d "
              "gap pixels)" % (st, fi), st > 60 and fi > 60)

        pv = D["PR_X"] + D["PR_W"] * D["PR_VALUE"] // 100
        tail = [px(x, D["PR_Y"] + 4) for x in range(pv + 12, pv + 60)]
        check("progress: spun as far as %d%% (%s), its knot's white there "
              "(%s), the stitch after it" % (D["PR_VALUE"],
                                              px(D["PR_X"] + 10, D["PR_Y"] + 5),
                                              px(pv, D["PR_Y"] + 5)),
              px(D["PR_X"] + 10, D["PR_Y"] + 5) == rgb(accent) and
              px(pv, D["PR_Y"] + 5) == (255, 255, 255) and
              rgb(C["PU_N_STITCH"]) in tail)

        lr = px(D["LST_X"] + D["LST_W"] - 12, D["LST_Y"] + 18 + 9)
        check("the list's chosen row: the accent faint behind it (%s)" % (lr,),
              lr == mix(accent, LOOK["NIGHT_APP_FIELD"],
                        ugui_define("PU_SEL_A")))

        # clicks
        click(sx + 20, s2 + 11)
        ok = wait_said(q, "[pudemo] changed Dark theme at night 1")
        shot()
        check("a click turns the second switch on: it says so, and its knob "
              "is white at its right", ok and
              px(sx + 29, s2 + 11) == (255, 255, 255))
        click((seg(2) + seg(3)) // 2, D["SEG_Y"] + D["SEG_H"] // 2)
        ok = wait_said(q, "[pudemo] changed View 2")
        shot()
        check("a click on the third segment: chosen, the thread under it",
              ok and px((seg(2) + seg(3)) // 2, foot) == rgb(accent) and
              px((seg(0) + seg(1)) // 2, foot) != rgb(accent))
        click(D["KN_X"] + 8, D["KN_Y"] + 2 * 24 + 12)
        ok = wait_said(q, "[pudemo] changed Loom size 2")
        shot()
        check("a click on the third knot: it is filled, the second hollow",
              ok and kc(2) == rgb(accent) and kc(1) == rgb(bg))
        click(D["CK_X"] + 8, D["CK2_Y"] + 9)
        check("a click ticks the second box",
              wait_said(q, "[pudemo] changed Send reports 1"))
        click(D["B0_X"] + 20, my)
        check("a click presses the button", wait_said(q, "[pudemo] pressed "
                                                         "Install"))

        # the keys
        q.mouse_goto(sw - 10, sh // 2)
        q.sendkey("tab")
        ok = wait_said(q, "[pudemo] focus Install")
        shot()
        check("Tab gives the keys to the first control, and the thread "
              "wraps round it two pixels out (%s)" % (px(D["B0_X"] - 3, my),),
              ok and px(D["B0_X"] - 3, my) == rgb(accent) and
              px(D["B0_X"] - 4, my) == rgb(accent))
        n = q.serial().count("[pudemo] pressed Install")
        q.sendkey("spc")
        end = time.time() + 10
        while time.time() < end and \
                q.serial().count("[pudemo] pressed Install") == n:
            time.sleep(0.3)
        check("Space presses it",
              q.serial().count("[pudemo] pressed Install") == n + 1)
        q.sendkey("tab")
        ok = wait_said(q, "[pudemo] focus Cancel")
        shot()
        check("Tab again: the thread moves to the next (%s, %s)"
              % (px(D["B1_X"] - 3, my), px(D["B0_X"] - 3, my)),
              ok and px(D["B1_X"] - 3, my) == rgb(accent) and
              px(D["B0_X"] - 3, my) == rgb(bg))
        click(D["FLD_X"] + 20, D["FLD_Y"] + 10)
        q.sendkey("x")
        ok = wait_said(q, "[pudemo] typed Computer name threados-pcx")
        shot()
        row = [px(x, fy) for x in range(D["FLD_X"] + 8,
                                        D["FLD_X"] + D["FLD_W"] - 8)]
        check("typing reaches the field, and with the keys its stitch is "
              "the thread (%d of %d pixels)" % (row.count(rgb(accent)),
                                                len(row)),
              ok and row.count(rgb(accent)) == len(row))

        # the Mac style: the same window, the old way
        q.click_desktop()
        q.sudo("set layout mac", "saved")
        check("the Mac style", wait_said(q, "the Mac style"))
        time.sleep(2)
        _, _, at = q.screen_raw()
        mox, moy = ox, found[1] + LOOK["TITLE_H"] + 1
        v1 = at(mox + D["B1_X"] + 5, moy + my)
        check("under the Mac style another thing is a row's grey again "
              "(%s)" % (v1,), v1 == rgb(LOOK["DARK_APP_ROW"]))
    finally:
        q.close()
        if os.path.exists(slog):
            os.unlink(slog)


def main():
    print("--- ProtonUI v2 ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    slog = tempfile.mktemp(suffix=".log")
    q = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog)
    try:
        q.wait_serial("thread64>", timeout=90)
        out = q.command("run pulook", "[pulook] done", timeout=60)
        check_look(out, "the PC", STYLE_BAR_DOCK, LOOK["DENSITY_POINTER"])
        check("the PC: the dark theme's thread is DARK_ACCENT",
              looks(out).get("night", [0] * 64)[18] == LOOK["DARK_ACCENT"])
    finally:
        q.close()
        if os.path.exists(slog):
            os.unlink(slog)
    marks()
    controls()
    return summary()


if __name__ == "__main__":
    sys.exit(main())
