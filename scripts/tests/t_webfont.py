#!/usr/bin/env python3
"""ThreadOS smoke test: a page's own fonts (@font-face).

The fonts are made here with fontTools, and their letters are
rectangles: 'a' is 400 units wide and 700 high with an advance of 500,
'b' is 900 wide with an advance of 1000, in 1000 units to the em. At
40 px a run "aab" is then exactly two rectangles of 16 x 28 pixels and
one of 36 x 28, at 0, 20 and 40 - what any correct renderer draws, with
nothing of a rasteriser's own judgement in it (the rasteriser itself is
held against FreeType by t_ttf).

  a .ttf named in an @font-face of a <style> draws the run, and the
  layout measures it with that font: what follows the run starts at 80

  a WOFF (each table zlib-compressed) the same, and a WOFF2 (all of it
  in one Brotli stream, the glyphs taken apart into streams of their
  own) - made by fontTools, with libbrotli (scripts/brotliref.py)

  a real font as WOFF2: the system's DejaVu Sans, glyf and hmtx
  transformed, sets a line exactly as wide as fontTools' advances for it
  say

  font-weight picks the face: of a family with a 400 and a 700, bold
  text gets the 700 one, whose 'a' is wider

  an @font-face in an external style sheet counts too, its url() taken
  against the sheet's address; and one font file named by two families
  is fetched once

Run on its own or through scripts/run_tests.py.
"""

import io
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webtest import *        # noqa: F401,F403
from webtest import check, run, visit
import ptest                 # noqa: E402
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import brotliref             # noqa: E402

try:
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen
except ImportError:
    FontBuilder = None

RED, GREEN, BLUE = (0xC0, 0x20, 0x20), (0x20, 0xA0, 0x30), (0x20, 0x40, 0xC0)
ORANGE, PURPLE = (0xD0, 0x80, 0x10), (0x80, 0x20, 0xA0)


def rect(w, h):
    pen = TTGlyphPen(None)
    if w and h:
        pen.moveTo((0, 0))
        pen.lineTo((0, h))
        pen.lineTo((w, h))
        pen.lineTo((w, 0))
        pen.closePath()
    return pen.glyph()


def woff2_of(ttf, tables=("glyf", "loca")):
    from fontTools.ttLib import woff2
    out = io.BytesIO()
    woff2.compress(io.BytesIO(ttf), out, transform_tables=set(tables))
    return out.getvalue()


DEJAVU = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))), "fonts", "sans.ttf")
REAL_TEXT = "Brotli unpacks fonts"


def make_font(name, glyphs, flavor=None):
    """glyphs: char -> (advance, width, height), in 1000 units an em"""
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder([".notdef"] + list(glyphs))
    fb.setupCharacterMap({ord(g): g for g in glyphs})
    fb.setupGlyf({".notdef": rect(0, 0), **{g: rect(*v[1:]) for g, v in glyphs.items()}})
    fb.setupHorizontalMetrics({".notdef": (500, 0), **{g: (v[0], 0) for g, v in glyphs.items()}})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": name, "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    fb.setupPost()
    if flavor:
        fb.font.flavor = flavor
    out = io.BytesIO()
    fb.font.save(out)
    return out.getvalue()


def line(cls, family, colour, text, weight=400):
    return (f"<p style='margin:0;font:{weight} 40px {family},sans-serif;"
            f"color:rgb{colour};line-height:36px'><span>{text}</span>"
            f"<span style='display:inline-block;width:6px;height:10px;"
            f"background:rgb{BLUE}' class={cls}></span></p>")


def fonts(e):
    q, pg = e.q, e.page
    if FontBuilder is None:
        check("fontTools is here to make the fonts (it is not: skipped)", True)
        return
    regular = {"a": (500, 400, 700), "b": (1000, 900, 700)}
    SERVED["/f/probe.ttf"] = make_font("Probe", regular)
    SERVED["/f/probe.woff"] = make_font("Probe", regular, "woff")
    SERVED["/f/bold.ttf"] = make_font("Probe", {"a": (700, 600, 700)})
    have_w2 = brotliref.install()
    if have_w2:
        SERVED["/f/probe.woff2"] = woff2_of(SERVED["/f/probe.ttf"])
        SERVED["/f/dejavu.woff2"] = woff2_of(open(DEJAVU, "rb").read(), ("glyf", "loca", "hmtx"))
    SERVED["/css/fonts.css"] = (b"@font-face{font-family:'ProbeX';"
                                b"src:url(../f/probe.ttf) format('truetype')}")
    SERVED["/fonts.html"] = (
        "<!doctype html><html><head><title>Fonts</title>"
        "<link rel=stylesheet href='/css/fonts.css'><style>"
        "@font-face{font-family:ProbeT;src:local('Nothing'),url(/f/probe.ttf) format('truetype')}"
        "@font-face{font-family:\"ProbeW\";src:url('/f/probe.woff') format('woff')}"
        "@font-face{font-family:ProbeB;font-weight:400;src:url(/f/probe.ttf)}"
        "@font-face{font-family:ProbeB;font-weight:700;src:url(/f/bold.ttf)}"
        "@font-face{font-family:Probe2;src:url(/f/probe.woff2) format('woff2')}"
        "@font-face{font-family:Real2;src:url(/f/dejavu.woff2) format('woff2')}"
        "</style></head><body style='margin:0'>" +
        line("t", "ProbeT", RED, "aab") + line("w", "ProbeW", GREEN, "aab") +
        line("r", "ProbeB", ORANGE, "a") + line("b", "ProbeB", PURPLE, "a", 700) +
        line("x", '"ProbeX"', RED, "b") + line("2", "Probe2", GREEN, "aab") +
        line("d", "Real2", ORANGE, REAL_TEXT) +
        "</body></html>").encode()

    r0 = len(ptest.REQUESTS)
    visit(e, "/fonts.html", "Web: Fonts")
    rows = {}
    end = time.time() + 20
    while time.time() < end:
        rows = {}
        for name, colour, top in [("t", RED, 0), ("w", GREEN, 36), ("r", ORANGE, 72),
                                  ("b", PURPLE, 108), ("x", RED, 144), ("2", GREEN, 180),
                                  ("d", ORANGE, 216)]:
            rows[name] = pixels(q, pg[0], pg[1] + top, pg[2], pg[1] + top + 36, colour)[0]
        if all(v for k, v in rows.items() if have_w2 or k not in "2d"):
            break
        time.sleep(0.5)

    def rel(box):
        return box and (box[0] - pg[0], box[2] - box[0] + 1, box[3] - box[1] + 1)

    def marker(top):
        box = pixels(q, pg[0], pg[1] + top, pg[2], pg[1] + top + 36, BLUE)[0]
        return box and box[0] - pg[0]

    t = rel(rows["t"])
    check(f"a .ttf of an @font-face draws the run: rectangles from 0 to 76, "
          f"28 high ({t})", t == (0, 76, 28))
    check(f"and the layout measures it so: what follows starts at 80 "
          f"({marker(0)})", marker(0) == 80)
    w = rel(rows["w"])
    check(f"a WOFF the same ({w}, then {marker(36)})", w == (0, 76, 28) and marker(36) == 80)
    r, b = rel(rows["r"]), rel(rows["b"])
    check(f"font-weight picks the face: 400 draws the 16 px 'a', 700 the "
          f"24 px one ({r}, {b})", r and b and r[1] == 16 and b[1] == 24 and
          marker(72) == 20 and marker(108) == 28)
    x = rel(rows["x"])
    asked = [r[2].split()[1] for r in ptest.REQUESTS[r0:]]
    check(f"an @font-face of an external sheet, its url() against the sheet "
          f"({x}); probe.ttf, named by three families, fetched once "
          f"({asked.count('/f/probe.ttf')})",
          x == (0, 36, 28) and asked.count("/f/probe.ttf") == 1)

    if not have_w2:
        check("libbrotli is here to make WOFF2 fonts with (it is not: skipped)", True)
        return
    w2 = rel(rows["2"])
    check(f"a WOFF2 the same ({w2}, then {marker(180)})", w2 == (0, 76, 28) and marker(180) == 80)
    from fontTools.ttLib import TTFont
    real = TTFont(DEJAVU)
    cmap, hmtx, upem = real.getBestCmap(), real["hmtx"], real["head"].unitsPerEm
    want = sum(hmtx[cmap[ord(c)]][0] for c in REAL_TEXT) * 40 / upem
    got = marker(216)
    check(f"DejaVu Sans as WOFF2 sets '{REAL_TEXT}' {got} px wide at 40 px, as "
          f"its advances say ({want:.1f})", got is not None and abs(got - want) <= 1.5)


def main():
    return run("a page's own fonts", [fonts])


if __name__ == "__main__":
    sys.exit(main())
