#!/usr/bin/env python3
"""ThreadOS smoke test: the loading screen of a boot to the desktop.

A boot that ends at the desktop shows a loading screen instead of the
kernel's text console, the way a Mac starts (kernel/bootsplash.c): black
over the whole framebuffer the loader set, ThreadOS's mark
(art/threados-mark.svg) large and white in the middle, and under it a
thin bar with round ends that fills as the boot goes on. No words; the
kernel's log goes to the serial port alone.

What is checked is what can be seen, while it can be seen: the screen
is read from the moment QEMU starts, over and over for as long as the
splash is up, until the desktop takes its place. Then, picture by
picture, at the places bootsplash.c's numbers put things:

  the mark, every pixel of its box, against FreeType's drawing of the
  same SVG read by fontTools' own parser - not by scripts/mklogo.py,
  whose coverage the kernel scales - into the same box

  the top of the screen black, every point of a grid over it - no log
  line, no uptime in its corner

  the bar: every length it was seen at is one of the steps kmain64.c
  names (its share of the way), each later one longer; its empty part
  white at bootsplash.c's strength over black

The old loading screen - the 8x8 bitmap blown up on flat navy - fails
the first check already.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, gfx_mix, _kernel_define

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
SRC = "kernel/bootsplash.c"
LOGO_D = _kernel_define(SRC, "SPLASH_LOGO_D")
LOGO_DY = _kernel_define(SRC, "SPLASH_LOGO_DY")
BAR_W = _kernel_define(SRC, "SPLASH_BAR_W")
BAR_H = _kernel_define(SRC, "SPLASH_BAR_H")
BAR_DY = _kernel_define(SRC, "SPLASH_BAR_DY")
TRACK_A = _kernel_define(SRC, "SPLASH_TRACK_A")
BLACK, WHITE = (0, 0, 0), (255, 255, 255)


def steps():
    """The steps the kernel names, as shares of the way"""
    out = set()
    for f in ("kernel64/kmain64.c", SRC):
        out |= {int(p) for p in re.findall(
            r'boot_splash_update\((\d+), "[^"]+"\)',
            open(os.path.join(ROOT, f)).read())}
    return sorted(out)


MARK_SVG = os.path.join(ROOT, "art", "threados-mark.svg")


def logo_size():
    """The coverage the kernel was built with: LOGO_W x LOGO_H, from the
    header scripts/mklogo.py wrote next to the kernel under test"""
    src = open(os.path.join(os.path.dirname(os.path.abspath(KERNEL)), "gen",
                            "logo.h")).read()
    return (int(re.search(r"#define LOGO_W (\d+)", src).group(1)),
            int(re.search(r"#define LOGO_H (\d+)", src).group(1)))


class Mark:
    """Where bootsplash.c's draw_mark() puts the mark on a w x h screen,
    and FreeType's drawing of the SVG into that box: rows of coverage"""

    def __init__(self, w, h):
        d = LOGO_D if h >= 400 else LOGO_D * 2 // 3
        lw, lh = logo_size()
        big = max(lw, lh)
        self.w, self.h = d * lw // big, d * lh // big
        # kernel/mark.c: the mark in the middle of its d x d box
        self.x0 = (w - d) // 2 + (d - self.w) // 2
        self.y0 = h // 2 - LOGO_DY - d // 2 + (d - self.h) // 2
        self.top = self.y0
        self.ref = reference(self.w, self.h)
        # Points the shape covers whole: where the mark must be white
        self.solid = [(self.x0 + c, self.y0 + r) for r in range(self.h)
                      for c in range(self.w) if self.ref[r][c] == 255][::97]


def reference(pw, ph):
    """art/threados-mark.svg drawn by FreeType into a pw x ph box: the SVG
    read by fontTools, its curves made quadratic, put into a glyph of a
    font whose pixel is 64 units, so the box's edges fall on pixels"""
    import freetype
    from fontTools.pens.boundsPen import BoundsPen
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.cu2quPen import Cu2QuPen
    from fontTools.pens.transformPen import TransformPen
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.svgLib.path import SVGPath
    # The mark's own bounds - the curves' extremes, not the viewBox's
    # margins - which kernel/mark.c's coverage is cut to
    bounds = BoundsPen(None)
    SVGPath(MARK_SVG).draw(bounds)
    x0, y0, x1, y1 = bounds.bounds
    vx, vy, vw, vh = x0, y0, x1 - x0, y1 - y0
    sx, sy = pw / vw * 64, ph / vh * 64
    glyph = TTGlyphPen(None)
    SVGPath(MARK_SVG).draw(TransformPen(
        Cu2QuPen(glyph, 1.0),
        (sx, 0, 0, -sy, -vx * sx, ph * 64 + vy * sy)))
    ppem = 64
    fb = FontBuilder(ppem * 64, isTTF=True)
    fb.setupGlyphOrder([".notdef", "mark"])
    fb.setupCharacterMap({0x41: "mark"})
    fb.setupGlyf({".notdef": TTGlyphPen(None).glyph(), "mark": glyph.glyph()})
    # The left side bearing is where the outline begins: FreeType puts a
    # TrueType outline by it, and a 0 there would move the mark left
    g = fb.font["glyf"]["mark"]
    g.recalcBounds(fb.font["glyf"])
    fb.setupHorizontalMetrics({".notdef": (0, 0), "mark": (pw * 64, g.xMin)})
    fb.setupHorizontalHeader(ascent=ph * 64, descent=0)
    fb.setupNameTable({"familyName": "ThreadOS mark", "styleName": "Regular"})
    fb.setupOS2()
    fb.setupPost()
    path = tempfile.mktemp(suffix=".ttf")
    fb.save(path)
    try:
        face = freetype.Face(path)
        face.set_pixel_sizes(0, ppem)
        face.load_char("A", freetype.FT_LOAD_RENDER | freetype.FT_LOAD_NO_HINTING)
        g = face.glyph
        bm = g.bitmap
        out = [[0] * pw for _ in range(ph)]
        for j in range(bm.rows):
            r = ph - g.bitmap_top + j
            for i in range(bm.width):
                c = g.bitmap_left + i
                if 0 <= r < ph and 0 <= c < pw:
                    out[r][c] = bm.buffer[j * bm.pitch + i]
        return out
    finally:
        os.unlink(path)


def near(a, b, tol=2):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    named = steps()
    check("kmain64.c names the steps of the boot (%d of them)" % len(named),
          len(named) >= 6)

    log = tempfile.mktemp(suffix=".log")
    q = Qemu(KERNEL, INITRD, serial=log)
    try:
        def splash_up(s):
            """Every corner black, the bar's far end there (empty or
            full), and white across the middle of the mark's box. Cheap:
            the boot does not wait while a picture is looked at, and the
            mark is held to FreeType's drawing afterwards."""
            w, h, at = s
            if w < 320:
                return False
            d = LOGO_D if h >= 400 else LOGO_D * 2 // 3
            bw = min(w - 64, BAR_W)
            end_x = (w - bw) // 2 + bw - BAR_H - 2
            mid = h // 2 + BAR_DY + BAR_H // 2
            row = h // 2 - LOGO_DY
            return (all(at(x, y) == BLACK for x, y in
                        ((2, 2), (w - 3, 2), (2, h - 3), (w - 3, h - 3)))
                    and at(end_x, mid) in (gfx_mix(WHITE, BLACK, TRACK_A),
                                           WHITE)
                    and any(at(x, row) == WHITE
                            for x in range((w - d) // 2, (w + d) // 2)))

        def shot():
            try:
                return q.screen_raw()
            except Exception:
                return None

        first = None
        end = time.time() + 60
        while time.time() < end and first is None:
            s = shot()
            if s and splash_up(s):
                first = s
            time.sleep(0.05)
        check("the splash covers the whole screen in black, the mark in "
              "its middle", first is not None)
        if first is None:
            return summary()
        w, h, _ = first

        # Watch it for as long as it is up; what it showed is read after
        shots = []
        s = first
        end = time.time() + 60
        while time.time() < end:
            if s and (s[0], s[1]) == (w, h) and splash_up(s):
                shots.append(s)
            elif s and s[0] >= 640 and not splash_up(s):
                break                      # the desktop is there
            s = shot()
            time.sleep(0.05)

        mark = Mark(w, h)
        at = shots[-1][2]
        worst = total = lit = 0
        for r in range(-4, mark.h + 4):
            for c in range(-4, mark.w + 4):
                want = mark.ref[r][c] if 0 <= r < mark.h and 0 <= c < mark.w \
                    else 0
                got = at(mark.x0 + c, mark.y0 + r)
                d = max(abs(g - want) for g in got)
                worst = max(worst, d)
                total += d
                lit += got[0] > 128
        n = (mark.w + 8) * (mark.h + 8)
        check("the mark is the SVG as FreeType draws it into its %dx%d box "
              "(and black around it): every pixel within 32 of 255, on "
              "average within 1 - worst %d, mean %.2f, %d pixels lit"
              % (mark.w, mark.h, worst, total / n, lit),
              lit > mark.w * mark.h // 8 and worst <= 32 and total / n <= 1)

        cy = h // 2
        grid = [(x, y) for y in range(4, mark.top - 12, 19)
                for x in range(3, w, 31)]
        dirty = sum(any(at(x, y) != BLACK for x, y in grid)
                    for _, _, at in shots)
        check("the top of the screen is black all over - no log line, no "
              "uptime in its corner (%d of %d looks differ, %d points each)"
              % (dirty, len(shots), len(grid)), shots and dirty == 0)

        bar_w = min(w - 64, BAR_W)
        bx, by = (w - bar_w) // 2, cy + BAR_DY
        mid = by + BAR_H // 2
        # How long the bar is in each picture, in the order they came; a
        # picture with none was taken between the bar being cleared and
        # drawn again, and says nothing
        seen = [sum(sh[2](x, mid) == WHITE for x in range(bx, bx + bar_w))
                for sh in shots]
        drawn = [n for n in seen if n]
        lengths = sorted(set(drawn))
        # the round end's last pixels are not all of the colour
        odd = [n for n in lengths
               if not any(abs(n - bar_w * p // 100) <= 3 for p in named)]
        check("the bar fills further as the boot goes on (%s pixels)"
              % lengths, len(lengths) >= 2 and drawn == sorted(drawn))
        check("each length it was seen at is one of the steps kmain64.c "
              "names (not: %s)" % (odd or "none"), lengths and not odd)

        x = bx + bar_w - BAR_H - 2
        tail = [shots[k][2](x, mid) for k, n in enumerate(seen)
                if 0 < n < bar_w - BAR_H - 6]
        want = gfx_mix(WHITE, BLACK, TRACK_A)
        check("the bar's empty part is white at %d of 255 over black "
              "(%s, wanted %s)" % (TRACK_A, tail[-1] if tail else None, want),
              tail and all(p == want for p in tail))

        serial = q.wait_serial("[boot] visual loading screen active",
                               timeout=30)
        check("the kernel reports the graphical boot path",
              "[boot] visual loading screen active" in serial)
        m = re.search(r"Framebuffer: (\d+)x(\d+)", serial)
        check("the splash fills the framebuffer the loader set (%dx%d, "
              "the kernel says %s)" % (w, h, m.group(0) if m else "nothing"),
              m is not None and (int(m.group(1)), int(m.group(2))) == (w, h))

        # The log goes on, on the serial port only, past the moment the
        # splash came up
        after = q.wait_serial("The 64-bit system is ready", timeout=60)
        check("the kernel's log carries on on the serial port",
              after.find("initrd:") >
              after.find("[boot] visual loading screen active"))

        check("and the desktop takes the splash's place",
              q.wait_desktop())
    finally:
        q.close()
        if os.path.exists(log):
            os.unlink(log)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
