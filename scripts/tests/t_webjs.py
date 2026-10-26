#!/usr/bin/env python3
"""ThreadOS smoke test: JavaScript in the browser.

A page with a script from a file and one of its own. What they do is
held to the screen and to what the browser says, not to the scripts'
word:

  the file's function is there for the page's script: the title it
  sets is the window's

  at DOMContentLoaded the script writes a paragraph's text and adds a
  paragraph of its own in a colour no style sheet names - it shows

  a click on a link runs the listener the script put on it: a class is
  added and the page's style sheet, matched afresh, paints the box; the
  link's own going is prevented

  a timer fires later and paints another box; console.log reaches the
  serial line

  a script that never ends is stopped, and the page shows anyway

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webtest import *        # noqa: F401,F403
from webtest import check, define, run, typed, visit


def scripts(e):
    q, port, sw, sh, w0, h0 = e.q, e.port, e.sw, e.sh, e.w0, e.h0
    geom, cx, cy, bar, page, step = e.geom, e.cx, e.cy, e.bar, e.page, e.step

    seen = len(q.serial())
    got = visit(e, "/js.html", "Web: Scripted 5")
    time.sleep(1)
    said = q.serial()[seen:]
    check(f"a function from a script file, called by the page's own "
          f"script, names the window ({got!r})", got == "Web: Scripted 5")
    check("console.log reaches the serial line, and both scripts ran",
          "[js] hello from the page" in said and "[js] ran 2 scripts" in said)
    made, n = pixels(q, *page, (0xAA, 0x00, 0x66))
    check(f"a paragraph made at DOMContentLoaded shows, in the colour the "
          f"script gave it ({n} pixels)", n > 40)
    q.sendkey("ctrl-f")
    time.sleep(0.5)
    typed(q, "1-4-9")
    end = time.time() + 10
    found = ""
    while time.time() < end and not found:
        found = next((ln for ln in reversed(q.serial()[seen:].splitlines())
                      if "[web] find" in ln), "")
        time.sleep(0.3)
    q.sendkey("esc")
    check(f"and the text it wrote is on the page ({found!r})",
          found.endswith("1 of 1"))

    hot = (0xFF, 0x88, 0x00)
    before, _ = pixels(q, *page, hot)
    link, _ = pixels(q, *page, (0x80, 0x60, 0x40))
    seen = len(q.serial())
    q.click((link[0] + link[2]) // 2, link[3] - 3)
    q.mouse_goto(sw - 20, sh // 2)
    time.sleep(1.5)
    after, n = pixels(q, *page, hot)
    title = q.window_title(*geom)
    check(f"a click runs the listener: a class added, and the page's style "
          f"sheet paints the box ({before} -> {after})",
          before is None and after is not None and
          after[2] - after[0] + 1 == 120)
    check(f"and preventDefault keeps the link from going ({title!r})",
          title == "Web: Scripted 5" and "/nowhere" not in q.serial()[seen:])
    late, _ = pixels(q, *page, (0x00, 0xAA, 0x88))
    check(f"a timer has painted the other box by now ({late})",
          late is not None and late[2] - late[0] + 1 == 90)

    seen = len(q.serial())
    got = visit(e, "/loop.html", "Web: Loop")
    said = q.serial()[seen:]
    check(f"a script that never ends is stopped, and its page shows "
          f"({got!r})", got == "Web: Loop" and "interrupted" in said)


def main():
    return run("JavaScript in the browser", [scripts])


sys.exit(main())
