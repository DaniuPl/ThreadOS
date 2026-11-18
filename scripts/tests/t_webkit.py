#!/usr/bin/env python3
"""ThreadOS smoke test: WebKit in a desktop window (64-bit kernel).

WPE WebKit's three processes on ThreadOS (Makefile, `make webkit`):
weft opens a window through the ThreadOS platform
(apps/webkit/wpethreados.c) and WebKit starts wpenetproc, which fetches
the page with libsoup, and wpewebproc, which parses it, runs its script
in JavaScriptCore, lays it out and paints it with Skia on the processor
into shared memory, which the window shows. They come from the
package weft.tapp (scripts/mktapp.py) the disc hands over beside the
initrd.

A page served from the host is loaded, and from outside:

  the page's script puts what it measured into its title, which
  weft prints: the boxes' places and sizes - absolutely placed ones,
  and three flex items sharing a row with a gap - and a number written
  the Polish way by Intl (ICU). The same page in Chromium (Playwright,
  on this machine) and Node's Intl have to say the same

  the window shows the page: the red box is 200 x 100 pixels of its
  exact colour, the blue one 120 x 120 with its corners rounded off,
  the flex items green, brown and green in the widths measured, the
  Polish sentence is drawn in dark pixels, and so are the markers of a
  bulleted and a numbered list - which need the system's font itself,
  not a fallback, since their size comes from its metrics

  a click into the text field, letters typed and a click on the button
  run the page's handler, which puts what was typed into the title

and then the browser around the page (apps/webkit/weft.c), driven
the way a person drives it and read off the screen:

  an address typed into the address field loads that page, and the
  field shows its address; back and forward go between the two

  a link that opens a new window opens a new tab, in front, named
  after its page; "+" opens a tab with the start page; a click on a tab
  brings its page back; Ctrl+W and a tab's close button close tabs, and
  closing the last ends the program with code 0

  the dock's Web tile starts this browser rather than the desktop's own
  one when the applications' image is there, with the cursor in the
  address field

  the mouse's wheel over a page taller than the window: two notches
  towards the person go down the page, and the screen shows it exactly
  as far as its script reads in scrollY; two away go back to the top

  with --proxy, a page is asked of the proxy: one on a host that does
  not exist arrives from the test's own proxy, which saw the request

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
from ptest import check, summary, make_iso64, no64, UD_PX
from ptest import ui_font_baseline_in as baseline_in
from ptest import _kernel_define as define

BUILD = os.path.dirname(os.path.abspath(KERNEL64 or "build/x"))
APPS = os.path.join(BUILD, "weft.tapp")
SRC = "apps/webkit/weft.c"
RED, BLUE = (0xC0, 0x30, 0x30), (0x33, 0x66, 0xCC)
GREEN, BROWN = (0x22, 0xAA, 0x44), (0x80, 0x60, 0x40)
PURPLE, TEAL = (0x80, 0x40, 0xC0), (0x20, 0x80, 0x80)
ORANGE, START = (0xC0, 0x80, 0x20), (0xF4, 0xF4, 0xF6)

# The second page: purple, with a link that opens a new window; the
# third, orange, is what the link opens
TWO = (b'<!doctype html><html><head><meta charset="utf-8"><title>two'
       b'</title><style>html,body{margin:0;height:100%;background:#8040c0}'
       b'#out{position:absolute;left:20px;top:20px;width:200px;'
       b'height:100px;background:#208080;display:block}</style></head>'
       b'<body><a id="out" href="/three.html" target="_blank"></a>'
       b'</body></html>')
# What the test's proxy answers with, whatever is asked of it
PROXIED = (b'<!doctype html><html><head><meta charset="utf-8"><title>'
           b'proxied</title><style>html,body{margin:0;height:100%;'
           b'background:#30a050}</style></head><body></body></html>')
PROXY_GREEN = (0x30, 0xA0, 0x50)
THREE = (b'<!doctype html><html><head><meta charset="utf-8"><title>three'
         b'</title><style>html,body{margin:0;height:100%;'
         b'background:#c08020}</style></head><body></body></html>')
# A page to scroll: a blue box to tap at the top, a green bar a little
# way down and a red band far below the first screen; the title says
# what was done to it - once the page has stood still for half a
# second, how far it scrolled
TALL = (b'<!doctype html><html><head><meta charset="utf-8"><title>tall'
        b'</title><style>html,body{margin:0;background:#ffffff}'
        b'body{height:6000px}#tap{position:absolute;left:20px;top:20px;'
        b'width:200px;height:100px;background:#3366cc}'
        b'#bar{position:absolute;left:0;top:300px;width:100%;'
        b'height:100px;background:#22aa44}'
        b'#band{position:absolute;left:0;top:1600px;width:100%;'
        b'height:200px;background:#c03030}</style></head><body>'
        b'<div id="tap"></div><div id="bar"></div><div id="band"></div>'
        b"<script>document.getElementById('tap').onclick = () => {"
        b"  document.title = 'tapped'; };"
        b"let still;"
        b"addEventListener('scroll', () => { clearTimeout(still);"
        b"  still = setTimeout(() => {"
        b"    document.title = 'scrolled ' + Math.round(scrollY); }, 500); });"
        b'</script></body></html>')
BAR_TOP, BAR_H = 300, 100
BAND_TOP, BAND_H = 1600, 200

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
#ul, #ol { position: absolute; left: 440px; margin: 0; padding-left: 40px;
           font: 20px sans-serif; color: #000000 }
#ul { top: 160px } #ol { top: 220px }
</style></head><body>
<div id="red"></div><div id="blue"></div>
<div id="row"><div></div><div class="two"></div><div></div></div>
<input id="field"><button id="go">Go</button>
<div id="text">Zażółć gęślą jaźń</div>
<ul id="ul"><li>one</li></ul><ol id="ol"><li>two</li></ol>
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


def tell(q, seen):
    """What the programs and the kernel said since 'seen', when a step
    did not happen: the lines worth reading, the last forty of them"""
    out = q.serial()[seen:].encode("latin1").decode("utf-8", "replace")
    keep = [l for l in out.replace("\r", "").split("\n")
            if re.search(r"weft|wpe|WebKit|GLib|Gtk|kernel\]|!!|thread64|"
                         r"error|ERROR|CRITICAL|WARNING|failed", l)]
    for line in keep[-40:]:
        print("   | " + line[:200])


def wait_colour(q, x, y, colour, timeout=60):
    """Polls the screen until the pixel is that colour: the frame comes
    when the web process has painted it, not at a moment known here"""
    end = time.time() + timeout
    got = None
    while time.time() < end:
        _, _, at = q.screen_raw()
        got = at(x, y)
        if got == colour:
            return True, got
        time.sleep(0.5)
    return False, got


class Band:
    """Where the browser's band is, from the page's origin on the screen
    and weft.c's own layout numbers"""

    def __init__(self, ox, oy):
        d = lambda n: define(SRC, n)
        self.ox, self.top = ox, oy - d("CHROME_H")
        self.tab_h, self.bar_h = d("TAB_H"), d("BAR_H")
        self.tab_w, self.close_w = d("TAB_W"), d("CLOSE_W")
        self.plus_w, self.width = d("PLUS_W"), d("WIDTH")
        self.btn_x, self.btn_w = d("BTN_X"), d("BTN_W")
        self.btn_step = d("BTN_STEP")
        self.addr_x, self.addr_h = d("ADDR_X"), d("ADDR_H")

    def tw(self, n):
        return min((self.width - self.plus_w - 8) // n, self.tab_w)

    def tab(self, i, n):
        """The middle of the i-th of n tabs, clear of its close button"""
        return (self.ox + 4 + i * self.tw(n) + (self.tw(n) - self.close_w) // 2,
                self.top + 3 + (self.tab_h - 3) // 2)

    def close(self, i, n):
        return (self.ox + 4 + i * self.tw(n) + self.tw(n) - 2 - self.close_w + 8,
                self.top + 3 + (self.tab_h - 3) // 2)

    def plus(self, n):
        return (self.ox + 4 + n * self.tw(n) + 4 + self.plus_w // 2,
                self.top + self.tab_h // 2)

    def button(self, k):
        """back (0), forward (1), reload (2)"""
        return (self.ox + self.btn_x + k * self.btn_step + self.btn_w // 2,
                self.top + self.tab_h + self.bar_h // 2)

    def field(self):
        """The address field at its right end: the pointer left over
        the address would cover the letters the test reads next"""
        return (self.ox + self.width - 8 - 20,
                self.top + self.tab_h + self.bar_h // 2)

    def address(self, q, want=None, timeout=20):
        """What the address field shows, read off the screen. The band
        is drawn by the program when a page says it moved on, so the
        screen is read until it shows 'want' or the time is up."""
        ay = self.top + self.tab_h + (self.bar_h - self.addr_h) // 2
        end = time.time() + timeout
        while True:
            got = q.read_ui(self.ox + self.addr_x + 10,
                            baseline_in(UD_PX, ay, self.addr_h),
                            width=self.width - self.addr_x - 30, px=UD_PX)
            if want is None or got == want or time.time() > end:
                return got
            time.sleep(0.5)

    def title(self, q, i, n):
        """A tab's title, read off the screen"""
        return q.read_ui(self.ox + 4 + i * self.tw(n) + 8,
                         baseline_in(UD_PX, self.top + 3, self.tab_h - 3),
                         width=self.tw(n) - 10 - self.close_w, px=UD_PX)


def browse(q, port, ox, oy):
    """The browser around the page: the address field, back and
    forward, tabs. The page at ox, oy is page.html, alone in its tab."""
    band = Band(ox, oy)
    base = f"http://10.0.2.2:{port}"
    mid = (ox + 320, oy + 300)            # in every page, below the boxes

    shown = band.address(q, base + "/page.html")
    check(f"the address field shows the page's address ({shown!r})",
          shown == base + "/page.html")

    seen = len(q.serial())
    q.click(*band.field())
    time.sleep(1)
    q.type_line(f"10.0.2.2:{port}/two.html")
    go = wait_line(q, seen, "[weft] go ", 30)
    check(f"an address typed into the field is taken as a web address "
          f"({go})", go == f"[weft] go {base}/two.html")
    done = wait_line(q, seen, f"[weft] load finished {base}/two.html", 120)
    ok, got = wait_colour(q, *mid, PURPLE)
    check(f"and that page loads into the window ({got})",
          done is not None and ok)
    shown = band.address(q, base + "/two.html")
    check(f"the field shows the new address ({shown!r})",
          shown == base + "/two.html")

    q.click(*band.button(0))
    ok, got = wait_colour(q, ox + 100, oy + 60, RED)
    check(f"back: the first page again, its red box where it was ({got})",
          ok)
    shown = band.address(q, base + "/page.html")
    check(f"and its address ({shown!r})", shown == base + "/page.html")
    q.click(*band.button(1))
    ok, got = wait_colour(q, *mid, PURPLE)
    check(f"forward: the second page again ({got})", ok)

    # A link that opens a new window: a tab, in front
    ok, got = wait_colour(q, ox + 100, oy + 60, TEAL)
    seen = len(q.serial())
    q.click(ox + 100, oy + 60)
    tab = wait_line(q, seen, "[weft] tab ", 30)
    done = wait_line(q, seen, f"[weft] load finished {base}/three.html",
                     120)
    ok, got = wait_colour(q, *mid, ORANGE)
    check(f"a link to a new window opens a second tab, in front, with its "
          f"page ({tab}; {got})",
          tab == "[weft] tab 2 of 2" and done is not None and ok)
    time.sleep(1)
    names = [band.title(q, 0, 2), band.title(q, 1, 2)]
    check(f"the tabs are named after their pages ({names})",
          names == ["two", "three"])

    seen = len(q.serial())
    q.click(*band.plus(2))
    tab = wait_line(q, seen, "[weft] tab ", 30)
    ok, got = wait_colour(q, *mid, START)
    check(f"'+' opens a third tab with the start page ({tab}; {got})",
          tab == "[weft] tab 3 of 3" and ok)

    seen = len(q.serial())
    q.click(*band.tab(0, 3))
    tab = wait_line(q, seen, "[weft] tab ", 30)
    ok, got = wait_colour(q, *mid, PURPLE)
    check(f"a click on the first tab brings its page back ({tab}; {got})",
          tab == "[weft] tab 1 of 3" and ok)

    seen = len(q.serial())
    q.click(*band.close(2, 3))
    tab = wait_line(q, seen, "[weft] tab ", 30)
    check(f"the close button of the last tab closes it; the first stays "
          f"in front ({tab})", tab == "[weft] tab 1 of 2")
    seen = len(q.serial())
    q.sendkey("ctrl-w")
    tab = wait_line(q, seen, "[weft] tab ", 30)
    ok, got = wait_colour(q, *mid, ORANGE)
    check(f"Ctrl+W closes the tab in front, and the other comes forward "
          f"({tab}; {got})", tab == "[weft] tab 1 of 1" and ok)
    seen = len(q.serial())
    q.sendkey("ctrl-w")
    end = wait_line(q, seen, "[weft] done, exit code", 60)
    check(f"closing the last tab ends the program, cleanly ({end})",
          end is not None
          and re.match(r"\[weft\] done, exit code 0\b", end))


def wheel(q, port, ox, oy):
    """The mouse's wheel over a page taller than the window: towards the
    person it goes down the page, and the screen shows the page where
    its script says it is; away from the person, back up"""
    band = Band(ox, oy)
    seen = len(q.serial())
    q.click(*band.field())
    time.sleep(1)
    q.type_line(f"10.0.2.2:{port}/tall.html")
    done = wait_line(q, seen,
                     f"[weft] load finished http://10.0.2.2:{port}/tall.html",
                     120)
    ok, _ = wait_colour(q, ox + 20, oy + BAR_TOP + 20, GREEN)
    check(f"the tall page loads, its green bar {BAR_TOP} pixels down",
          done is not None and ok)
    q.mouse_goto(ox + 300, oy + 300)
    seen = len(q.serial())
    q.mouse_wheel(-2)
    line = wait_line(q, seen, "[weft] title scrolled ", 60)
    moved = int(line.split()[-1]) if line else 0
    check(f"two notches towards the person scroll down the page ({line})",
          0 < moved < BAR_TOP)
    want = (ox, oy + BAR_TOP - moved)
    got = None
    end = time.time() + 30
    while got != want and time.time() < end:
        sw, sh, at = q.screen_raw()
        bar = find_box(at, sw, sh, GREEN)
        got = bar[:2] if bar else None
        time.sleep(0.5)
    check(f"and the screen shows it {moved} pixels up: the bar at {got}, "
          f"where the page puts it {want}", got == want)
    seen = len(q.serial())
    q.mouse_wheel(2)
    line = wait_line(q, seen, "[weft] title scrolled ", 60)
    check(f"two away from the person, back to the top ({line})",
          line == "[weft] title scrolled 0")


def from_dock(q, port):
    """The dock's Web tile: this browser, waiting for an address"""
    seen = len(q.serial())
    q.launch_dock("web", n_w=0)
    tab = wait_line(q, seen, "[weft] tab ", 120)
    check(f"the dock's Web tile starts the WebKit browser ({tab})",
          tab == "[weft] tab 1 of 1")
    page = None
    end = time.time() + 120
    while page is None and time.time() < end:
        sw, sh, at = q.screen_raw()
        page = find_box(at, sw, sh, START)
        if page is None or page[2] - page[0] < 600:
            page = None
            time.sleep(1)
    check(f"with the start page in its window ({page})", page is not None)
    seen = len(q.serial())
    q.type_line(f"10.0.2.2:{port}/two.html")
    done = wait_line(q, seen, "[weft] load finished http", 120)
    ok = page is not None and wait_colour(
        q, page[0] + 320, page[1] + 300, PURPLE)[0]
    check(f"and the cursor in the address field: an address typed there "
          f"loads ({done})",
          done == f"[weft] load finished http://10.0.2.2:{port}/two.html"
          and ok)
    if page is not None:
        wheel(q, port, page[0], page[1])
    # Its only tab closed, it ends - and until it has, the keys typed
    # next would still be its window's, not the shell's
    seen = len(q.serial())
    q.sendkey("ctrl-w")
    end = wait_line(q, seen, "[kernel] process 'weft' has ended", 60)
    check(f"Ctrl+W on its only tab ends it ({end})",
          end == "[kernel] process 'weft' has ended (code 0)")


def start_proxy():
    """A web proxy of the test's own. A browser asks a proxy for a page
    by its whole address; this one answers every such request with a
    page of its own and keeps what was asked"""
    import http.server
    import threading
    asked = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            asked.append(self.path)
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(PROXIED)))
            self.end_headers()
            self.wfile.write(PROXIED)

        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server.server_address[1], asked, server.shutdown


def via_proxy(q):
    """--proxy: a page on a host no name server knows reaches the
    window only if the browser asked the proxy for it"""
    pport, asked, stop = start_proxy()
    try:
        page = "http://threados.invalid/through-the-proxy"
        # The keys to the shell: the window of the browser just closed
        # goes from the screen a frame after its program ends
        q.click_desktop()
        time.sleep(1)
        seen = len(q.serial())
        q.type_line(f"run weft --proxy http://10.0.2.2:{pport} {page}")
        title = wait_line(q, seen, "[weft] title ", 180)
        check(f"with --proxy the page comes from the proxy ({title})",
              title == "[weft] title proxied")
        if title != "[weft] title proxied":
            tell(q, seen)
        check(f"which was asked for it by its whole address ({asked})",
              page in asked)
        page_box = None
        end = time.time() + 60
        while page_box is None and time.time() < end:
            sw, sh, at = q.screen_raw()
            page_box = find_box(at, sw, sh, PROXY_GREEN)
            if page_box is None:
                time.sleep(1)
        check(f"and it is on the screen ({page_box})",
              page_box is not None and page_box[2] - page_box[0] >= 600)
        q.sendkey("ctrl-w")
        wait_line(q, seen, "[weft] done, exit code", 60)
    finally:
        stop()


def main():
    print()
    print("--- WebKit in a desktop window (64-bit kernel) ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    if not os.path.exists(APPS):
        print("SKIPPED: no weft.tapp - make webkit")
        return summary()
    width, height = define(SRC, "WIDTH"), define(SRC, "HEIGHT")
    want = chromium_title(width, height)
    if want is None:
        print("NOTE: no Chromium through Playwright on this machine - "
              "the layout is held to the numbers worked out by hand")
        want = ("ready red=20,20,200,100 blue=260,20,120,120 "
                "row=20+95 125+190 325+95 num=" + (node_number() or "?"))
    num = node_number()

    # As a web server says it - a page served as bytes of no known type
    # is a download, which a browser does not show
    for path, body in (("/page.html", PAGE), ("/two.html", TWO),
                       ("/three.html", THREE), ("/tall.html", TALL)):
        ROUTES[path] = (lambda b: lambda h, body: (
            200, [("Content-Type", "text/html; charset=utf-8")], b))(body)
    port, stop = start_http_server()
    work = tempfile.mkdtemp()
    iso = os.path.join(work, "t.iso")
    try:
        check("the disc with the kernel and the applications' image builds",
              make_iso64(os.path.join(work, "iso"), iso, graphics=True,
                         tapps=[APPS]))
        q = Qemu(KERNEL, INITRD, cdrom=iso,
                 serial=os.path.join(work, "serial.log"),
                 qemu="qemu-system-x86_64", ram_mib=2048)
        try:
            q.wait_serial("thread64>", timeout=120)
            q.wait_serial("[net] DHCP", timeout=60)
            check("the kernel takes Weft's package",
                  re.search(r"Package weft \d+: Weft, ", q.serial())
                  is not None)
            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=30)
            check("the desktop comes up", q.wait_desktop())
            q.mouse_home()
            seen = len(q.serial())
            q.type_line(f"run weft http://10.0.2.2:{port}/page.html")

            done = wait_line(q, seen, "[weft] load finished", 300)
            check(f"WebKit fetches and loads the page ({done})",
                  done is not None and "/page.html" in done)
            if done is None or "/page.html" not in done:
                time.sleep(5)
                tell(q, seen)
            title = wait_line(q, seen, "[weft] title ready", 120)
            got = title[len("[weft] title "):] if title else None
            check(f"the page's script ran and measured the page: ThreadOS "
                  f"{got!r}, Chromium {want!r}", got == want)
            check(f"Intl wrote the number the Polish way, as Node's ICU does "
                  f"({num!r})", got is not None and num is not None and
                  got.endswith("num=" + num))
            frame = wait_line(q, seen, "[weft] frame shown", 120)
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
            # The markers stand in the lists' 40 pixels of padding, left
            # of the items' text: nothing else is drawn there
            marks = [sum(1 for y in range(oy + top, oy + top + 30)
                         for x in range(ox + 440, ox + 478)
                         if sum(at(x, y)) < 300)
                     for top in (160, 220)]
            check(f"the list markers are drawn: a bullet and '1.' "
                  f"({marks} dark pixels)", marks[0] > 10 and marks[1] > 20)

            seen = len(q.serial())
            q.click(ox + 120, oy + 255)                # the text field
            time.sleep(1)
            q.type_line("abc", enter=False)
            time.sleep(1)
            q.click(ox + 310, oy + 255)                # the button
            clicked = wait_line(q, seen, "[weft] title clicked", 60)
            check(f"a click, letters typed and a click on the button reach "
                  f"the page's handler ({clicked})",
                  clicked == "[weft] title clicked abc")

            browse(q, port, ox, oy)
            from_dock(q, port)
            via_proxy(q)
        finally:
            q.close()
    finally:
        stop()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
