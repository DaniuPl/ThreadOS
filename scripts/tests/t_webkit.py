#!/usr/bin/env python3
"""ThreadOS smoke test: WebKit in a desktop window (64-bit kernel).

WPE WebKit's three processes on ThreadOS (Makefile, `make webkit`):
webview opens a window through the ThreadOS platform
(apps/webkit/wpethreados.c) and WebKit starts wpenetproc, which fetches
the page with libsoup, and wpewebproc, which parses it, runs its script
in JavaScriptCore, lays it out and paints it with Skia on the processor
into shared memory, which the window shows. They come from the
applications' image (apps64.img) the disc hands over beside the initrd.

A page served from the host is loaded, and from outside:

  the page's script puts what it measured into its title, which
  webview prints: the boxes' places and sizes - absolutely placed ones,
  and three flex items sharing a row with a gap - and a number written
  the Polish way by Intl (ICU). The same page in Chromium (Playwright,
  on this machine) and Node's Intl have to say the same

  the window shows the page: the red box is 200 x 100 pixels of its
  exact colour, the blue one 120 x 120 with its corners rounded off,
  the flex items green, brown and green in the widths measured, and
  the Polish sentence is drawn in dark pixels

  a click into the text field, letters typed and a click on the button
  run the page's handler, which puts what was typed into the title

Run on its own or through scripts/run_tests.py.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64
from ptest import _kernel_define as define

BUILD = os.path.dirname(os.path.abspath(KERNEL64 or "build/x"))
APPS = os.path.join(BUILD, "apps64.img")
SRC = "apps/webkit/webview.c"
RED, BLUE = (0xC0, 0x30, 0x30), (0x33, 0x66, 0xCC)
GREEN, BROWN = (0x22, 0xAA, 0x44), (0x80, 0x60, 0x40)

SCRIPT = r"""
const r = id => { const b = document.getElementById(id)
                  .getBoundingClientRect();
                  return [b.left, b.top, b.width, b.height].join(','); };
const cells = [...document.querySelectorAll('#row div')]
  .map(d => { const b = d.getBoundingClientRect();
              return b.left + '+' + b.width; }).join(' ');
const num = new Intl.NumberFormat('pl-PL').format(1234567.891);
document.title = 'ready red=' + r('red') + ' blue=' + r('blue') +
  ' row=' + cells + ' num=' + num;
document.getElementById('go').onclick = () => {
  document.title = 'clicked ' + document.getElementById('field').value; };
"""

PAGE = ("""<!doctype html>
<html><head><meta charset="utf-8"><title>loading</title><style>
html, body { margin: 0; background: #ffffff }
#red { position: absolute; left: 20px; top: 20px; width: 200px;
       height: 100px; background: #c03030 }
#blue { position: absolute; left: 260px; top: 20px; width: 120px;
        height: 120px; background: #3366cc; border-radius: 24px }
#row { position: absolute; left: 20px; top: 160px; width: 400px;
       height: 40px; display: flex; gap: 10px }
#row div { flex: 1; background: #22aa44 }
#row div.two { flex: 2; background: #806040 }
#field { position: absolute; left: 20px; top: 240px; width: 200px;
         height: 30px; font: 16px sans-serif }
#go { position: absolute; left: 260px; top: 240px; width: 100px;
      height: 30px }
#text { position: absolute; left: 20px; top: 300px; font: 24px sans-serif;
        color: #000000 }
</style></head><body>
<div id="red"></div><div id="blue"></div>
<div id="row"><div></div><div class="two"></div><div></div></div>
<input id="field"><button id="go">Go</button>
<div id="text">Zażółć gęślą jaźń</div>
<script>""" + SCRIPT + """</script>
</body></html>
""").encode("utf-8")


def chromium_title(width, height):
    """The title the same page gets in Chromium, at the window's size:
    None without Playwright"""
    js = (
        "const { chromium } = require('playwright');"
        "(async () => {"
        " const b = await chromium.launch();"
        " const p = await b.newPage({ viewport: { width: %d, height: %d } });"
        " await p.setContent(process.argv[1]);"
        " await p.waitForFunction(() => document.title.startsWith('ready'));"
        " console.log(JSON.stringify(await p.title()));"
        " await b.close();"
        "})().catch(e => { console.error(e); process.exit(1); });"
        % (width, height))
    try:
        p = subprocess.run(["node", "-e", js, PAGE.decode("utf-8")],
                           capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if p.returncode:
        return None
    return json.loads(p.stdout.strip().splitlines()[-1])


def node_number():
    """1234567.891 the Polish way, by Node's Intl"""
    try:
        p = subprocess.run(["node", "-e", "console.log(new Intl.NumberFormat"
                            "('pl-PL').format(1234567.891))"],
                           capture_output=True, text=True, timeout=30)
    except OSError:
        return None
    return p.stdout.strip() if p.returncode == 0 else None


def find_box(at, w, h, colour, area=None):
    """The bounding box of the pixels of exactly this colour: x0, y0,
    x1, y1 (exclusive), or None"""
    x0, y0, x1, y1 = area or (0, 0, w, h)
    xs, ys = [], []
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            if at(x, y) == colour:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    # the even grid found it; the edges exactly
    bx0, by0, bx1, by1 = min(xs), min(ys), max(xs), max(ys)
    while bx0 > 0 and at(bx0 - 1, (by0 + by1) // 2) == colour:
        bx0 -= 1
    while bx1 + 1 < w and at(bx1 + 1, (by0 + by1) // 2) == colour:
        bx1 += 1
    while by0 > 0 and at((bx0 + bx1) // 2, by0 - 1) == colour:
        by0 -= 1
    while by1 + 1 < h and at((bx0 + bx1) // 2, by1 + 1) == colour:
        by1 += 1
    return bx0, by0, bx1 + 1, by1 + 1


def wait_line(q, seen, prefix, timeout):
    """The first line after 'seen' on the serial port that starts with
    the prefix, or None"""
    end = time.time() + timeout
    while time.time() < end:
        out = q.serial()[seen:].encode("latin1").decode("utf-8", "replace")
        for line in out.replace("\r", "").split("\n"):
            if line.startswith(prefix):
                return line
        time.sleep(1)
    return None


def main():
    print()
    print("--- WebKit in a desktop window (64-bit kernel) ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    if not os.path.exists(APPS):
        print("SKIPPED: no apps64.img - make webkit")
        return summary()
    width, height = define(SRC, "WIDTH"), define(SRC, "HEIGHT")
    want = chromium_title(width, height)
    if want is None:
        print("NOTE: no Chromium through Playwright on this machine - "
              "the layout is held to the numbers worked out by hand")
        want = ("ready red=20,20,200,100 blue=260,20,120,120 "
                "row=20+95 125+190 325+95 num=" + (node_number() or "?"))
    num = node_number()

    SERVED["/page.html"] = PAGE
    port, stop = start_http_server()
    work = tempfile.mkdtemp()
    iso = os.path.join(work, "t.iso")
    try:
        check("the disc with the kernel and the applications' image builds",
              make_iso64(os.path.join(work, "iso"), iso, graphics=True,
                         apps=APPS))
        q = Qemu(KERNEL, INITRD, cdrom=iso,
                 serial=os.path.join(work, "serial.log"),
                 qemu="qemu-system-x86_64", ram_mib=2048)
        try:
            q.wait_serial("thread64>", timeout=120)
            q.wait_serial("[net] DHCP", timeout=60)
            check("the kernel takes the applications' image",
                  "Applications' image:" in q.serial())
            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=30)
            check("the desktop comes up", q.wait_desktop())
            q.mouse_home()
            seen = len(q.serial())
            q.type_line(f"run webview http://10.0.2.2:{port}/page.html")

            done = wait_line(q, seen, "[webview] load finished", 300)
            check(f"WebKit fetches and loads the page ({done})",
                  done is not None and "/page.html" in done)
            title = wait_line(q, seen, "[webview] title ready", 120)
            got = title[len("[webview] title "):] if title else None
            check(f"the page's script ran and measured the page: ThreadOS "
                  f"{got!r}, Chromium {want!r}", got == want)
            check(f"Intl wrote the number the Polish way, as Node's ICU does "
                  f"({num!r})", got is not None and num is not None and
                  got.endswith("num=" + num))
            frame = wait_line(q, seen, "[webview] frame shown", 120)
            check("a frame of the page reaches the window", frame is not None)

            time.sleep(2)
            sw, sh, at = q.screen_raw()
            red = find_box(at, sw, sh, RED)
            check(f"the red box is on the screen, 200 x 100 ({red})",
                  red is not None and red[2] - red[0] == 200 and
                  red[3] - red[1] == 100)
            if red is None:
                return
            ox, oy = red[0] - 20, red[1] - 20          # the page's origin
            blue = find_box(at, sw, sh, BLUE,
                            (ox + 250, oy + 10, ox + 390, oy + 150))
            check(f"the blue box beside it, 120 x 120 ({blue})",
                  blue == (ox + 260, oy + 20, ox + 380, oy + 140))
            check("with its corners rounded off: the corner pixel is not "
                  f"blue ({at(ox + 260, oy + 20)}), the middle of its edge "
                  f"is ({at(ox + 320, oy + 20)})",
                  at(ox + 260, oy + 20) != BLUE and
                  at(ox + 320, oy + 20) == BLUE)
            row = [at(ox + x, oy + 180) for x in (20, 114, 125, 314, 325, 419)]
            check(f"the flex items green, brown and green, 95, 190 and 95 "
                  f"pixels wide ({row})",
                  row == [GREEN, GREEN, BROWN, BROWN, GREEN, GREEN] and
                  at(ox + 117, oy + 180) == (255, 255, 255))
            dark = sum(1 for y in range(oy + 300, oy + 340)
                       for x in range(ox + 20, ox + 320)
                       if sum(at(x, y)) < 200)
            check(f"the Polish sentence is drawn ({dark} dark pixels)",
                  dark > 300)

            seen = len(q.serial())
            q.click(ox + 120, oy + 255)                # the text field
            time.sleep(1)
            q.type_line("abc", enter=False)
            time.sleep(1)
            q.click(ox + 310, oy + 255)                # the button
            clicked = wait_line(q, seen, "[webview] title clicked", 60)
            check(f"a click, letters typed and a click on the button reach "
                  f"the page's handler ({clicked})",
                  clicked == "[webview] title clicked abc")
        finally:
            q.close()
    finally:
        stop()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
