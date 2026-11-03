#!/usr/bin/env python3
"""ThreadOS smoke test: how the browser fetches what a page is made of.

  a page's pictures are fetched together, several at once - the server
  here answers each after 0.4 s, and they are seen to overlap - over
  connections kept for the next request (keep-alive): eight pictures
  come over fewer connections than there are pictures, and well before
  eight of those waits one after the other would end

  among them, a picture behind a redirect and one that is no picture: the
  rest are shown all the same; the pictures go with the jar's cookie

  what a page is made of is kept: a second page with the same style
  sheet and pictures asks the server for none of them again, and shows
  them; the reload button fetches them afresh

Run on its own or through scripts/run_tests.py.
"""

import os
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webtest import *        # noqa: F401,F403
from webtest import check, run, visit
import ptest                 # noqa: E402

WAIT = 0.4
COLOURS = [(0xC0, 0x20, 0x20), (0x20, 0xA0, 0x30), (0x20, 0x40, 0xC0),
           (0xD0, 0x80, 0x10), (0x80, 0x20, 0xA0), (0x10, 0x90, 0x90),
           (0x90, 0x90, 0x10), (0x50, 0x50, 0x50)]
SHEET = (0x30, 0x60, 0x90)


def solid_png(w, h, rgb):
    raw = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


COOKIES = []


def r_slow(handler, body):
    """A picture, after a wait - the colour is the number in its name"""
    COOKIES.append(handler.headers.get("Cookie") or "")
    time.sleep(WAIT)
    n = int(handler.path.split("/")[-1].split(".")[0])
    return 200, [("Content-Type", "image/png")], solid_png(40, 20, COLOURS[n])


def r_moved(handler, body):
    return 302, [("Location", "/slow/7.png")], b""


def r_set(handler, body):
    return 200, [("Set-Cookie", "seen=yes; Path=/"), ("Content-Type", "text/html")], \
        b"<html><head><title>Set</title></head><body>set</body></html>"


def page(title, n, moved=True):
    imgs = "".join(f"<img src='/slow/{i}.png' width=40 height=20> " for i in range(n))
    if moved:
        imgs += "<img src='/moved.png' width=40 height=20> <img src='/none.png' width=40 height=20>"
    return (f"<!doctype html><html><head><title>{title}</title>"
            f"<link rel=stylesheet href='/shared.css'></head><body style='margin:4px'>"
            f"<div class=box></div>{imgs}</body></html>").encode()


def fetches(e):
    q, pg = e.q, e.page
    ROUTES.update({"/slow": r_slow, "/moved.png": r_moved, "/setck": r_set})
    for i in range(len(COLOURS)):
        ROUTES[f"/slow/{i}.png"] = r_slow
    SERVED["/shared.css"] = b".box{width:120px;height:16px;background:#306090}"
    SERVED["/many.html"] = page("Many", 7)
    SERVED["/again.html"] = page("Again", 4, moved=False)

    visit(e, "/setck", "Web: Set")
    r0, c0 = len(ptest.REQUESTS), len(COOKIES)
    visit(e, "/many.html", "Web: Many")
    end, shown = time.time() + 20, []
    while time.time() < end:
        shown = [c for c in COLOURS if pixels(q, *pg, c)[0] is not None]
        if len(shown) == len(COLOURS):
            break
        time.sleep(0.5)
    check(f"all the pictures show, the one behind a redirect too, though one "
          f"is no picture ({len(shown)} of {len(COLOURS)})", len(shown) == len(COLOURS))
    reqs = [r for r in ptest.REQUESTS[r0:] if " /slow/" in r[2]]
    check(f"each picture is asked for once ({len(reqs)} for 8)", len(reqs) == 8)
    # the most in progress at one moment: at each start, those begun and
    # not yet answered
    most = max((sum(1 for o in reqs if o[0] <= r[0] < o[1]) for r in reqs), default=0)
    check(f"they are fetched several at once: {most} at the same time", most >= 3)
    conns = {r[3] for r in ptest.REQUESTS[r0:] if " /slow/" in r[2] or " /moved" in r[2]
             or " /none" in r[2]}
    check(f"over connections kept between them: {len(conns)} connections for "
          f"10 requests", 1 <= len(conns) <= 4)
    span = (max(r[1] for r in reqs) - min(r[0] for r in reqs)) if reqs else 99
    check(f"all in {span:.1f} s, not the {8 * WAIT:.1f} s of one after another",
          span < 8 * WAIT * 0.6)
    sent = COOKIES[c0:]
    check(f"the jar's cookie goes with every picture ({sent.count('seen=yes')} of "
          f"{len(sent)})", sent and all("seen=yes" in c for c in sent))

    # --- a second page made of the same --------------------------------
    r1 = len(ptest.REQUESTS)
    visit(e, "/again.html", "Web: Again")
    time.sleep(1)
    asked = [r[2] for r in ptest.REQUESTS[r1:]]
    shown = [c for c in COLOURS[:4] if pixels(q, *pg, c)[0] is not None]
    sheet = pixels(q, *pg, SHEET)[0]
    check(f"a page with the same style sheet and pictures asks for none of them "
          f"again, and shows them ({asked}, {len(shown)} pictures, sheet {sheet})",
          asked == ["GET /again.html HTTP/1.1"] and len(shown) == 4 and sheet is not None)

    r2 = len(ptest.REQUESTS)
    q.click(e.cx + 10 + 2 * e.step + 14, e.cy + e.bar // 2)      # reload
    q.mouse_goto(e.sw - 20, e.sh // 2)
    end = time.time() + 20
    while time.time() < end:
        asked = [r[2].split()[1] for r in ptest.REQUESTS[r2:]]
        if len([a for a in asked if a.startswith("/slow/")]) >= 4 and "/shared.css" in asked:
            break
        time.sleep(0.5)
    check(f"reload fetches them afresh ({sorted(set(asked))})",
          "/again.html" in asked and "/shared.css" in asked and
          sorted(a for a in asked if a.startswith("/slow/")) ==
          [f"/slow/{i}.png" for i in range(4)])


def main():
    return run("what a page is made of, fetched", [fetches])


if __name__ == "__main__":
    sys.exit(main())
