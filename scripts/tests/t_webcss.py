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
  fixed: position: sticky and position: fixed
  media: @media width tests and prefers-color-scheme

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
# the probes the browser is held to; the rest show what is still missing
HELD = ["var", "calc", "overflow", "fixed", "media"]


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
