#!/usr/bin/env python3
"""ThreadOS smoke test: SVG pictures in the browser.

A page with three: an .svg file shown at twice its own size, another at
its own, and an <svg> written into the page itself, an icon of 24 units
shown at 96 pixels on a coloured background. Each is held, pixel by
pixel, against librsvg - GNOME's renderer, which shares no line with
ours - drawing the same picture at the same size:

  the scaled one is drawn at the size it is shown at, not blown up
  from a small drawing - its edges as sharp as librsvg's

  the gradients come out as librsvg's do

  the inline one keeps its viewBox (HTML's parser lowers the case of
  its names, and SVG's have capitals), and what is transparent in it
  shows the background it stands on, not a white box

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from webtest import *        # noqa: F401,F403
from webtest import check, run, visit
import svgref                # noqa: E402
from svgcases import CASES   # noqa: E402

CASE = {name: (w, h, svg) for name, w, h, svg in CASES}
ICON = CASE["icon"][2].split("?>", 1)[1].split("-->", 1)[1]
INLINE = ICON.replace('width="24" height="24"',
                      'width="96" height="96" style="display:block"')

PAGE = ("""<html><head><title>Pictures</title></head><body style="margin:8px">
<div style="display:inline-block;vertical-align:top;border:2px solid #123456">
<img src="/shapes.svg" width="240" height="160" style="display:block"></div>
<div style="display:inline-block;vertical-align:top;border:2px solid #654321">
<img src="/gradients.svg" style="display:block"></div>
<div style="display:inline-block;vertical-align:top;border:2px solid #345612;
background:#206040">""" + INLINE + """</div>
</body></html>""").encode()

BACK = (0x20, 0x60, 0x40)        # dark: an edge laid on white would show


def inside(q, page, colour):
    """The box a 2-pixel border of this colour goes round"""
    box, _ = pixels(q, *page, colour)
    if not box:
        return None
    return (box[0] + 2, box[1] + 2, box[2] - 1, box[3] - 1)


def shown(q, box):
    """What the screen shows in a box: bytes of RGB"""
    _, _, at = q.screen_raw()
    out = bytearray()
    for y in range(box[1], box[3]):
        for x in range(box[0], box[2]):
            out += bytes(at(x, y))
    return bytes(out)


def on(rgba, back):
    out = bytearray(len(rgba) // 4 * 3)
    for i in range(len(rgba) // 4):
        a = rgba[i * 4 + 3]
        for k in range(3):
            out[i * 3 + k] = (rgba[i * 4 + k] * a + back[k] * (255 - a) + 127) // 255
    return bytes(out)


def pictures(e):
    q, page = e.q, e.page
    SERVED["/svg.html"] = PAGE
    SERVED["/shapes.svg"] = CASE["shapes"][2].encode()
    SERVED["/gradients.svg"] = CASE["gradients"][2].encode()
    got = visit(e, "/svg.html", "Web: Pictures")
    check(f"the page with the pictures opens ({got!r})", got == "Web: Pictures")
    time.sleep(2)

    if not svgref.available():
        check("librsvg is here to compare with (it is not: skipped)", True)
        return
    for name, frame, w, h, back in (
            ("shapes", (0x12, 0x34, 0x56), 240, 160, (255, 255, 255)),
            ("gradients", (0x65, 0x43, 0x21), 120, 60, (255, 255, 255)),
            ("inline icon", (0x34, 0x56, 0x12), 96, 96, BACK)):
        box = inside(q, page, frame)
        size = box and (box[2] - box[0], box[3] - box[1])
        if not box or size != (w, h):
            check(f"{name}: shown in a box of {w}x{h} ({size})", False)
            continue
        svg = {"shapes": CASE["shapes"][2], "gradients": CASE["gradients"][2],
               "inline icon": INLINE}[name]
        ref = on(svgref.render(svg.encode(), w, h), back)
        mean, far = svgref.compare(shown(q, box), ref)
        check(f"{name}: as librsvg draws it at {w}x{h} (mean difference "
              f"{mean:.2f}, {far * 100:.2f}% of pixels far off)",
              mean < 2.5 and far < 0.01)
        if name == "inline icon":
            _, n = pixels(q, *box, BACK)
            check(f"what is transparent in it shows the background under it "
                  f"({n} of {w * h} pixels)", n > w * h // 2)


def main():
    return run("SVG pictures in the browser", [pictures])


sys.exit(main())
