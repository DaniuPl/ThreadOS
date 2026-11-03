#!/usr/bin/env python3
"""ThreadOS smoke test: the browser lays CSS out where Chromium does.

Each probe (cssprobes.py) is a page of coloured boxes with no text, so
fonts do not come into it; where each colour lands on the screen is
held against where it lands in Chromium (cssref.json, made by
scripts/cssref.py), to within a pixel:

  var: custom properties, inherited, overridden lower down, and
       var(--missing, fallback)
  calc: calc() with %, rem, em and px mixed, nested brackets, min(),
       max() and clamp() - the percentages taken of the containing
       block's width
  overflow: overflow: hidden and overflow: clip cut a child off at the
       box's edge
  hidden: what sites hide text for screen readers with - clip: rect()
       and clip-path: inset() - visibility: hidden and opacity: 0
  transform: translate(), translateX() and translateY(), in px and in %
       of the box's own size, on a fixed box too
  fixed: position: sticky and position: fixed; an absolute inline-flex
       box is taken out of its line
  flexgap: gap between flex items, in a row and in a column
  grid, grid2, grid3: grid-template-columns and -rows with px, fr,
       repeat(), auto-fill and auto-fit with minmax(), named lines,
       negative line numbers, span, grid-auto-rows, auto placement
       around placed items, justify-/align-items and -self
  areas: grid-template-areas, items placed by area name out of order
  selectors: :not(), :is(), :where(), @supports, :has() and :enabled
  images: data: addresses, base64 and %-escaped, as a background and
       as a mask-image - drawn as the mask's own picture here
  media: @media width tests, calc() in them, and prefers-color-scheme

Run on its own or through scripts/run_tests.py.
"""

import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webtest import *        # noqa: F401,F403
from webtest import check, run, visit
import cssprobes             # noqa: E402

REF = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "cssref.json")))
HELD = [n for n, _ in cssprobes.PROBES]


def far(ours, ref):
    """Colours missing on one side or more than a pixel away"""
    bad = []
    for col in sorted(set(ours) | set(ref)):
        a, b = ours.get(col), ref.get(col)
        if a is None or b is None or any(abs(p - r) > 1 for p, r in zip(a[:4], b[:4])):
            bad.append((col, a and tuple(a[:4]), b and tuple(b[:4])))
    return bad


def probes(e):
    q = e.q
    for n, body in cssprobes.PROBES:
        SERVED[f"/probe/{n}.html"] = cssprobes.page(body)
    for n in HELD:
        visit(e, f"/probe/{n}.html", "Web: Probe")
        region = (e.page[0], e.page[1], e.page[0] + cssprobes.WIDTH,
                  e.page[1] + cssprobes.HEIGHT)
        # poll until the boxes are drawn, not once after a fixed wait
        ours, bad = {}, ["nothing yet"]
        for _ in range(20):
            w, h, at = q.screen_raw()
            ours = cssprobes.boxes(w, h, at, region)
            bad = far(ours, REF[n])
            if not bad:
                break
            time.sleep(0.5)
        check(f"probe '{n}' lays out as Chromium does "
              f"({len(REF[n])} boxes; off: {bad})", REF[n] and not bad)


def main():
    return run("CSS laid out as Chromium does", [probes])


if __name__ == "__main__":
    sys.exit(main())
