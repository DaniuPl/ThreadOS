"""The web browser's tests, shared: the pages, the server, the session

t_web, t_webform and t_webtools each boot the 64-bit kernel, open the
browser and go through their part of what it does. What they share -
the pages the host serves, the colours they are found by on the
screen, the boot and the browser's place on it - lives here, once.
"""

import os
import re
import shutil
import sys
import tempfile
import time
from types import SimpleNamespace

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
# A page that takes the rest from elsewhere: a style sheet of its own,
# a picture drawn at twice its size, and enough lines to scroll
def three(title):
    return (b"<html><head><title>" + title + b"</title>"
            b"<link rel=stylesheet href=style.css></head><body>"
            b"<img src=pic.png width=128 height=96>"
            b"<div class=ext>styled from a file</div>"
            b"<p><a href='#end' style='color:#806040'>to the end</a></p>" +
            b"".join(b"<p>line %d</p>" % i for i in range(40)) +
            b"<h2 id=end style='color:#aa22cc'>The end</h2>"
            b"</body></html>")


THREE = three(b"Three")
PURPLE = (0xAA, 0x22, 0xCC)
# (the link to it in a colour of its own: the picture's blue is LINK's)
BROWN = (0x80, 0x60, 0x40)
RESULTS = (b"<html><head><title>Results</title></head><body>"
           b"<p>found it</p></body></html>")
STYLE = b".ext{background:#22aa44;color:white;padding:6px;width:150px}"
GREEN_BOX = (0x22, 0xAA, 0x44)
THUMB = (0x9A, 0x9A, 0xA0)
# Forms: the field, the box and the button each in a colour of its own,
# for the test to find them by
FIELD, TICK_BG, GO_BG = (0xFF, 0xF0, 0xD0), (0xD0, 0xF0, 0xFF), (0xC0, 0xFF, 0xC0)
FORM = (b"<html><head><title>Form</title><style>"
        b".f{background:#fff0d0} .t{background:#d0f0ff} .g{background:#c0ffc0}"
        b"</style></head><body>"
        b"<form action=/find><p><input class=f name=q placeholder='Type here'>"
        b"<input type=hidden name=lang value=en> "
        b"<input class=t type=checkbox name=all checked> "
        b"<input class=g type=submit value=Go></p></form>"
        b"<form action=/other method=post><p><input name=x value=1> "
        b"<button style='background:#ffc0ff' name=b value=ok>Send</button>"
        b"</p></form></body></html>")
PINK = (0xFF, 0xC0, 0xFF)
# A form that POSTs: a list to choose from and a text of several lines
SELECT_BG, AREA_BG, CHOSEN = (0xFF, 0xE0, 0xE0), (0xE0, 0xFF, 0xE0), (0x33, 0x66, 0xCC)
POSTFORM = (b"<html><head><title>Post form</title></head><body>"
            b"<form action=/post method=post><p>"
            b"<select style='background:#ffe0e0' name=size>"
            b"<option value=s>Small</option><option value=m selected>Medium"
            b"</option><option value=l>Large</option></select> "
            b"<textarea style='background:#e0ffe0' name=note cols=20 rows=3>"
            b"first</textarea> "
            b"<input class=g style='background:#c0ffc0' type=submit "
            b"value=Post></p></form></body></html>")


# A site to sign in to: the login answers with a cookie and a redirect in
# one response, as real ones do - plus a cookie for another path and one
# for a domain this host is not in, neither of which may come back
LOGIN = (b"<html><head><title>Login</title></head><body>"
         b"<form action=/login method=post><p>"
         b"<input style='background:#fff0d0' name=user> "
         b"<input type=submit value='Sign in'></p></form></body></html>")
COOKIES = []          # (path, the Cookie header the browser sent)
# Ctrl+F: three matches, the last far down; plain text with markup in it;
# a file to download
FIND = (b"<html><head><title>Find</title></head><body><p>An apple a day.</p>" +
        b"".join(b"<p>filler %d</p>" % i for i in range(50)) +
        b"<p>apple pie</p>" +
        b"".join(b"<p>more %d</p>" % i for i in range(20)) +
        b"<p>last APPLE</p></body></html>")
ORANGE = (0xFF, 0x96, 0x32)
NOTES = b"<b>not bold</b> & plain\n"
REPORT = b"downloaded-ok: line one\ndownloaded-ok: line two\n" \
         b"downloaded-ok: line three\n"
DL = (b"<html><head><title>Downloads</title></head><body>"
      b"<p><a href=files/report.dat style='color:#806040'>the report</a>"
      b"</p></body></html>")
# Characters: the same Polish words in three single-byte sets, told by a
# <meta>, by the server, or by nothing (then the system's default for its
# language - English here, so windows-1252); Python's codecs say what
# the bytes must become
PL = "Zażółć gęślą jaźń"
LATIN = "Café crème brûlée"


def page_in(text, codec, meta=""):
    return ("<html><head>" + meta + "<title>" + text + "</title></head>"
            "<body><p>" + text + "</p></body></html>").encode(codec)


def r_pl_http(h, body):
    return 200, [("Content-Type", "text/html; charset=ISO-8859-2")], \
        page_in(PL, "iso-8859-2")


def r_pl_txt(h, body):
    return 200, [("Content-Type", "text/plain; charset=windows-1250")], \
        (PL + "\n").encode("cp1250")


def on_serial(text):
    """How UTF-8 text looks in q.serial(), which reads bytes as latin1"""
    return text.encode("utf-8").decode("latin1")


# Words to select and copy, and two fields to Tab between and paste into
VIOLET, FIELD2 = (0x8A, 0x2B, 0xE2), (0xE0, 0xE0, 0xFF)
COPY = (b"<html><head><title>Copy</title></head><body>"
        b"<p style='color:#8a2be2;font-size:20px'>copy these words</p>"
        b"<form action=/pasted><p><input style='background:#fff0d0' name=t> "
        b"<input style='background:#e0e0ff' name=u></p></form></body></html>")


def r_login(h, body):
    if body != b"user=daniu":
        return 403, [], b"no"
    return 302, [("Location", "/home"),
                 ("Set-Cookie", "session=abc123; Path=/; HttpOnly"),
                 ("Set-Cookie", "elsewhere=1; Path=/other"),
                 ("Set-Cookie", "evil=1; Domain=example.com")], b""


def r_home(h, body):
    got = h.headers.get("Cookie")
    COOKIES.append(("/home", got))
    title = b"Signed in" if got == "session=abc123" else b"Signed out"
    return 200, [("Content-Type", "text/html")], (
        b"<html><head><title>" + title + b"</title></head><body>"
        b"<p><img src=/avatar.png width=32 height=24> hello</p></body></html>")


def r_avatar(h, body):
    COOKIES.append(("/avatar.png", h.headers.get("Cookie")))
    return 200, [("Content-Type", "image/png")], make_png(16, 12)


def r_logout(h, body):
    return 302, [("Location", "/home"),
                 ("Set-Cookie", "session=; Path=/; Max-Age=0")], b""


def page_titled(t):
    return b"<html><head><title>" + t + b"</title></head><body></body></html>"


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


def colours(q, box, want):
    """How many pixels in a box are of colours 'want' says yes to"""
    _, _, at = q.screen_raw()
    return sum(1 for y in range(box[1], box[3] + 1)
               for x in range(box[0], box[2] + 1) if want(at(x, y)))


def typed(q, text):
    for ch in text:
        q.sendkey(Qemu.KEYNAMES.get(ch, ch))


def serve():
    """Every page the browser tests ask the host's server for"""
    port, stop = start_http_server()
    SERVED["/page.html"] = PAGE
    SERVED["/two.html"] = TWO
    SERVED["/three.html"] = THREE
    SERVED["/style.css"] = STYLE
    SERVED["/pic.png"] = make_png(64, 48)
    SERVED["/s?q=hello+world"] = RESULTS
    SERVED["/form.html"] = FORM
    SERVED["/post.html"] = POSTFORM
    SERVED["/login.html"] = LOGIN
    SERVED["/copy.html"] = COPY
    SERVED["/pl-meta.html"] = page_in(PL, "cp1250",
                                      "<meta charset=windows-1250>")
    SERVED["/latin.html"] = page_in(LATIN, "cp1252")
    ROUTES.update({"/pl-http.html": r_pl_http, "/pl.txt": r_pl_txt})
    SERVED["/find.html"] = FIND
    SERVED["/notes.txt"] = NOTES
    SERVED["/dl.html"] = DL
    SERVED["/files/report.dat"] = REPORT
    SERVED["/pasted?t=a&u=copy+these+words"] = page_titled(b"Pasted")
    ROUTES.update({"/login": r_login, "/home": r_home,
                   "/avatar.png": r_avatar, "/logout": r_logout})
    SERVED["POST /post size=l&note=first%0D%0Asecond"] = page_titled(b"Posted")
    SERVED["/find?q=two+words&lang=en&all=on"] = page_titled(b"Found")
    SERVED["/find?q=abc&lang=en"] = page_titled(b"Found plain")
    SERVED["POST /other x=1&b=ok"] = page_titled(b"Sent")
    return port, stop


def run(title, sections, search=False):
    """Boots the 64-bit kernel with the desktop, opens the browser on its
    start page and hands each section the session: e.q, the browser's
    place (e.cx, e.cy, e.geom, e.page ...) and the server's e.port.
    With 'search', the 'search' setting first points at the server."""
    print()
    print("--- %s ---" % title)
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    port, stop = serve()
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
            if search:
                # Words typed as an address go to the 'search' setting's
                # engine: here, the server on the host - set from the
                # terminal, where the profile made at setup is the admin
                q.launch_dock("gterm", w=sw, h=sh)
                time.sleep(3)
                q.click(cx + 200, cy + 150)
                q.type_line("set search http://10.0.2.2:%d/s?q=" % port)
                time.sleep(2)
                q.sendkey("alt-f4")
                time.sleep(2)
                q.n_win -= 1
            q.launch_dock("gweb", w=sw, h=sh)            # the "Web" tile
            got = wait_title(q, geom, "Web: Start")
            check(f"the Web tile opens the browser on its start page "
                  f"({got!r})", got == "Web: Start")
            e = SimpleNamespace(q=q, port=port, sw=sw, sh=sh, w0=w0, h0=h0,
                                wx=wx, wy=wy, geom=geom, cx=cx, cy=cy,
                                bar=bar, page=page,
                                step=define(SRC, "BTN_STEP"))
            for section in sections:
                section(e)
        finally:
            q.close()
    finally:
        stop()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


def go(e, path, title):
    """Types an address of the host's server into the browser and waits
    for the page's title: what the title bar said"""
    e.q.click(e.cx + define(SRC, "ADDR_X") + 60, e.cy + e.bar // 2)
    e.q.mouse_goto(e.sw - 20, e.sh // 2)
    time.sleep(1)
    typed(e.q, f"http://10.0.2.2:{e.port}{path}")
    e.q.sendkey("ret")
    got = wait_title(e.q, e.geom, title)
    time.sleep(1)
    return got
