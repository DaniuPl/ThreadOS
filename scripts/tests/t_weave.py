#!/usr/bin/env python3
"""ThreadOS smoke test: Weave, ThreadOS's own desktop style.

'layout weave' (Settings, Desktop & Dock, "Interface style"; the design
in docs/WEAVE.md, the plan in docs/protonui-plan.md stage 3): opaque
cloth with a woven grain instead of glass, the window in front marked
by one thread of colour, the loom along the bottom in place of a dock
or a taskbar. Read off the screen and done with the mouse, against
kernel/include/look.h's numbers and the grain's and the warp's
definitions computed here:

  the loom stands clear of the screen's edges, Night's cloth with the
  grain over it to the pixel; below it, the warp
  the warp: Night's ground and its 6x6 weave to the pixel where no
  thread crosses, and the person's thread (the accent) through its
  curve's middle
  a program started from the loom: its window carries the selvedge, the
  thread colour three pixels deep along its top, and its title bar is
  Night's title cloth with the grain; under its program on the loom the
  long piece of thread
  a second program: the selvedge moves to it, and the first window's
  bar is the cloth of a window without focus
  a desktop's knot on the loom takes the screen there: no window, the
  knot filled
  a program asking for the look hears the Weave style, the grain's
  ink and Night's colours
  back to 'layout mac', the bar is at the top again

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
WARP = []


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

            _, _, at = q.screen_raw()
            top = H - L["LOOM_GAP"] - L["LOOM_H"]
            # a strip of the loom between the spool and the programs,
            # under the icons' row: cloth and grain to the pixel
            lx = L["LOOM_GAP"] + L["LOOM_SLOT"] + 12
            wrong = patch_is(at, lx, top + 3, 40, 4,
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

            # Files, from the loom
            n = len(DOCK_APPS)
            prog_x = (W - n * L["LOOM_SLOT"]) // 2
            mid_y = top + L["LOOM_H"] // 2
            q.click(prog_x + DOCK_APPS.index("gfiles") * L["LOOM_SLOT"] +
                    L["LOOM_SLOT"] // 2, mid_y)
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
                wrong = patch_is(at, mx, y0 + L["SELVEDGE_H"] + 1, 30,
                                 L["TITLE_H"] - L["SELVEDGE_H"] - 2,
                                 lambda x, y: grain(L["NIGHT_TITLE_BG"],
                                                    x, y))
                check("its title bar is Night's title cloth with the grain "
                      "(%d wrong)" % len(wrong), not wrong)
                # the long piece of thread under the program in front
                n2 = n + 1
                prog_x2 = (W - n2 * L["LOOM_SLOT"]) // 2
                slot = prog_x2 + n * L["LOOM_SLOT"]
                ty = H - L["LOOM_GAP"] - 7
                run = sum(at(x, ty + 1) == accent
                          for x in range(slot, slot + L["LOOM_SLOT"]))
                check("on the loom, under the window's tile, the long piece "
                      "of thread (%d pixels)" % run,
                      run >= L["LOOM_THREAD_FOCUS"] - 2)

                # Settings in front of it: the selvedge moves
                q.click(prog_x2 + DOCK_APPS.index("gsettings") *
                        L["LOOM_SLOT"] + L["LOOM_SLOT"] // 2, mid_y)
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
                    q.mouse_goto((nx0 + nx1) // 2, ny0 + L["TITLE_H"] // 2)
                    q.mouse_button(True)
                    q.mouse_goto((nx0 + nx1) // 2,
                                 ny0 + L["TITLE_H"] // 2 + 160)
                    q.mouse_button(False)
                    q.mouse_goto(W // 2, H // 2)
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
                    # outline, between its corners
                    free = list(range(x0 + 12, x1 - 12, 3))
                    wrong = [(x, y) for x in free
                             for y in range(y0 + 1, y0 + L["SELVEDGE_H"])
                             if at(x, y) != grain(L["NIGHT_TITLE_OFF"], x,
                                                  y)]
                    check("and the first window's bar is the cloth of one "
                          "without focus, its selvedge gone (%d pixels of "
                          "it looked at, %d wrong)" % (len(free), len(wrong)),
                          free and not wrong)

            # a desktop's knot
            kx = lambda i: (W - L["LOOM_GAP"] - L["TB_SHOW_W"] - 6 -
                            L["TB_CLOCK_W"] - 2 * L["TB_TRAY_W"] - 16 -
                            (4 - i) * (L["LOOM_KNOT_D"] + 8))
            ky = top + (L["LOOM_H"] - L["LOOM_KNOT_D"]) // 2
            q.click(kx(1) + L["LOOM_KNOT_D"] // 2, ky + L["LOOM_KNOT_D"] // 2)
            q.mouse_goto(W // 2, 20)
            ok = False
            end = time.time() + 10
            while time.time() < end and not ok:
                time.sleep(0.7)
                _, _, at = q.screen_raw()
                ok = not selvedges(at, W, H, accent) and \
                    at(kx(1) + L["LOOM_KNOT_D"] // 2,
                       ky + L["LOOM_KNOT_D"] // 2) == accent
            check("a desktop's knot takes the screen there: no window, its "
                  "knot filled", ok)

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
