#!/usr/bin/env python3
"""ThreadOS smoke test: the web browser in a desktop window (64-bit kernel).

apps/browser/web.cpp is litehtml with a document_container that draws
through the desktop's own lettering. The group 'browser' holds the
layout to the host's; this one holds the window to what the page asked
for, from outside:

  the Web tile opens it on its start page, and the title bar says so

  an address typed after a click in the address field (the click picks
  the old address whole, so typing replaces it) fetches a page from a
  server on the host; the title bar takes the page's <title>

  the page's own CSS reaches the screen in its exact colours: the red
  heading, the floated blue box on the right half - with its words
  readable on it - and the blue underline of the link

  a click on that underline follows the link to the second page, and
  the back arrow returns to the first

Run on its own or through scripts/run_tests.py.
"""

import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64
from ptest import _kernel_define as define

SRC = "apps/browser/web.cpp"
RED, BLUE, LINK = (0xC0, 0x30, 0x30), (0x33, 0x66, 0xCC), (0x00, 0x00, 0xFF)

PAGE = b"""<html><head><title>Test page</title><style>
body{margin:16px;font-family:sans-serif} h1{color:#c03030;font-size:30px}
.box{background:#3366cc;color:white;padding:8px;width:200px;float:right}
</style></head><body><h1>Hello from litehtml</h1>
<div class=box>A floated blue box</div>
<p>Some <b>bold</b> text and a <a href="two.html">link to page two</a>.</p>
</body></html>"""
TWO = (b"<html><head><title>Page two</title></head><body>"
       b"<h2>Second page</h2><p>You followed the link.</p></body></html>")


def wait_title(q, geom, want, timeout=40):
    wx, wy, ww = geom
    got = ""
    end = time.time() + timeout
    while time.time() < end:
        got = q.window_title(wx, wy, ww)
        if got == want:
            break
        time.sleep(0.5)
    return got


def pixels(q, x0, y0, x1, y1, colour):
    """Where exactly this colour lies in a region: its bounding box and
    how many pixels"""
    _, _, at = q.screen_raw()
    xs, ys = [], []
    for y in range(y0, y1):
        for x in range(x0, x1):
            if at(x, y) == colour:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None, 0
    return (min(xs), min(ys), max(xs), max(ys)), len(xs)


def main():
    print()
    print("--- the web browser in a window (the 64-bit kernel) ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    port, stop = start_http_server()
    SERVED["/page.html"] = PAGE
    SERVED["/two.html"] = TWO
    work = tempfile.mkdtemp()
    iso = os.path.join(work, "t.iso")
    disk = os.path.join(work, "disk.img")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        check("the disc with the 64-bit kernel builds",
              make_iso64(os.path.join(work, "iso"), iso, graphics=True))
        q = Qemu(KERNEL, INITRD, cdrom=iso, disk=disk, sata=True,
                 serial=os.path.join(work, "serial.log"),
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=90)
            q.wait_serial("[net] DHCP", timeout=60)
            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=30)
            check("the desktop comes up", q.wait_desktop(30))
            sw, sh = q.size()
            w0, h0 = define(SRC, "W0"), define(SRC, "H0")
            wx, wy = cascade(2, w0 + 2, h0 + TITLE_H + 2, w=sw, h=sh)
            geom = (wx, wy, w0 + 2)
            cx, cy = wx + 1, wy + CONTENT_DY
            bar = define(SRC, "BAR_H")
            page = (cx, cy + bar, cx + w0, cy + h0 - define(SRC, "STATUS_H"))

            q.mouse_home()
            q.launch_dock(5, w=sw, h=sh)            # the "Web" tile
            got = wait_title(q, geom, "Web: Start")
            check(f"the Web tile opens the browser on its start page "
                  f"({got!r})", got == "Web: Start")

            # A click in the address field picks what is there: typing
            # replaces it rather than adding to it
            q.click(cx + define(SRC, "ADDR_X") + 60, cy + bar // 2)
            q.mouse_goto(sw - 20, sh // 2)
            time.sleep(1)
            for ch in f"http://10.0.2.2:{port}/page.html":
                q.sendkey(Qemu.KEYNAMES.get(ch, ch))
            q.sendkey("ret")
            got = wait_title(q, geom, "Web: Test page")
            check(f"the typed address was fetched from the host and the "
                  f"title bar took the page's <title> ({got!r})",
                  got == "Web: Test page")
            check("the program says what it opened",
                  f"[web] opened http://10.0.2.2:{port}/page.html"
                  in q.serial())

            mid = (page[0] + page[2]) // 2
            box, n_red = pixels(q, *page, RED)
            check(f"the heading is in the page's red, #c03030 exactly "
                  f"({n_red} pixels)", n_red > 150 and box and
                  box[1] < page[1] + 80)
            box, n_blue = pixels(q, *page, BLUE)
            bw = box[2] - box[0] + 1 if box else 0
            check(f"the floated box is #3366cc, 216 pixels wide (200 and "
                  f"the padding) on the right half ({box}, {n_blue} pixels)",
                  box is not None and bw == 216 and box[0] > mid and
                  box[2] > page[2] - 30)
            said = q.read_page(box[0] + 1, box[1], box[2], box[3] + 1,
                               define(SRC, "FONT_PX"), (255, 255, 255),
                               want="A floated blue box") if box else ""
            check(f"its words are white on it and read back ({said!r})",
                  said == "A floated blue box")

            link, n_link = pixels(q, page[0], page[1], mid, page[3], LINK)
            check(f"the link is drawn in the link blue with its underline "
                  f"({link}, {n_link} pixels)",
                  link is not None and n_link > 100 and
                  link[2] - link[0] > 60)

            # The underline is the link's bottom row: a click just above
            # it is on the words
            q.click((link[0] + link[2]) // 2, link[3] - 3)
            q.mouse_goto(sw - 20, sh // 2)
            got = wait_title(q, geom, "Web: Page two")
            check(f"a click on the link follows it ({got!r})",
                  got == "Web: Page two")

            q.click(cx + define(SRC, "BACK_W") // 2 + 10, cy + bar // 2)
            q.mouse_goto(sw - 20, sh // 2)
            got = wait_title(q, geom, "Web: Test page")
            check(f"the back arrow returns to the first page ({got!r})",
                  got == "Web: Test page")
        finally:
            q.close()
    finally:
        stop()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
