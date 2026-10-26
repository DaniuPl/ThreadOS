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
  the back arrow returns to the first; forward goes there again

  words typed as an address are searched for, with the engine the
  'search' setting names; a link to '#end' scrolls the page to it, and
  reload fetches a page the server has changed

  a picture is drawn at the size the page asks for, a style sheet from
  a file of its own is applied, and a long page has a scroll thumb

  a form: its field takes the keys after a click, and Enter or a button
  sends what the form holds - the server answers only to exactly the
  query expected

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
    SERVED["/three.html"] = THREE
    SERVED["/style.css"] = STYLE
    SERVED["/pic.png"] = make_png(64, 48)
    SERVED["/s?q=hello+world"] = RESULTS
    SERVED["/form.html"] = FORM
    SERVED["/post.html"] = POSTFORM
    SERVED["/login.html"] = LOGIN
    SERVED["/copy.html"] = COPY
    SERVED["/pasted?t=a&u=copy+these+words"] = page_titled(b"Pasted")
    ROUTES.update({"/login": r_login, "/home": r_home,
                   "/avatar.png": r_avatar, "/logout": r_logout})
    SERVED["POST /post size=l&note=first%0D%0Asecond"] = page_titled(b"Posted")
    SERVED["/find?q=two+words&lang=en&all=on"] = page_titled(b"Found")
    SERVED["/find?q=abc&lang=en"] = page_titled(b"Found plain")
    SERVED["POST /other x=1&b=ok"] = page_titled(b"Sent")
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
            step = define(SRC, "BTN_STEP")
            q.click(cx + 10 + step + 14, cy + bar // 2)
            q.mouse_goto(sw - 20, sh // 2)
            got = wait_title(q, geom, "Web: Page two")
            check(f"and the forward arrow goes there again ({got!r})",
                  got == "Web: Page two")
            q.click(cx + 10 + 14, cy + bar // 2)
            q.mouse_goto(sw - 20, sh // 2)
            wait_title(q, geom, "Web: Test page")

            # --- words, not an address: a search ------------------------
            q.click(cx + define(SRC, "ADDR_X") + 60, cy + bar // 2)
            q.mouse_goto(sw - 20, sh // 2)
            time.sleep(1)
            for ch in "hello world":
                q.sendkey(Qemu.KEYNAMES.get(ch, ch))
            q.sendkey("ret")
            got = wait_title(q, geom, "Web: Results")
            check(f"words typed as an address are searched for, with the "
                  f"engine the setting names ({got!r})",
                  got == "Web: Results")

            # --- a picture, a style sheet, and a long page --------------
            q.click(cx + define(SRC, "ADDR_X") + 60, cy + bar // 2)
            q.mouse_goto(sw - 20, sh // 2)
            time.sleep(1)
            for ch in f"http://10.0.2.2:{port}/three.html":
                q.sendkey(Qemu.KEYNAMES.get(ch, ch))
            q.sendkey("ret")
            got = wait_title(q, geom, "Web: Three")
            check(f"a page with a picture and a style sheet opens ({got!r})",
                  got == "Web: Three")
            time.sleep(1)
            quarters = {}
            for name, colour in (("red", (255, 0, 0)), ("green", (0, 255, 0)),
                                 ("blue", (0, 0, 255))):
                quarters[name] = pixels(q, *page, colour)
            sizes = {k: (v[0][2] - v[0][0] + 1, v[0][3] - v[0][1] + 1)
                     if v[0] else None for k, v in quarters.items()}
            check("the PNG is drawn at twice its size: each quarter 64x48 "
                  "of its exact colour (%s)" % sizes,
                  all(v == (64, 48) for v in sizes.values()) and
                  all(v[1] == 64 * 48 for v in quarters.values()))
            red, green = quarters["red"][0], quarters["green"][0]
            check("in its place: red top left, green beside it",
                  red and green and green[0] == red[2] + 1 and
                  green[1] == red[1])
            box, n = pixels(q, *page, GREEN_BOX)
            check(f"the style sheet in a file of its own was applied "
                  f"({box}, {n} pixels)",
                  box is not None and box[2] - box[0] + 1 == 162)
            thumb, n = pixels(q, page[2] - 12, page[1], page[2], page[3],
                              THUMB)
            check(f"a long page has a scroll thumb at the right ({thumb})",
                  thumb is not None and thumb[1] < page[1] + 10)
            q.sendkey("pgdn")
            time.sleep(1.5)
            moved, n = pixels(q, page[2] - 12, page[1], page[2], page[3],
                              THUMB)
            check(f"and PgDn moves it down ({thumb} -> {moved})",
                  thumb and moved and moved[1] > thumb[1] + 20)

            # --- a link to a place on the same page ----------------------
            q.sendkey("home")
            time.sleep(1.5)
            box, n = pixels(q, *page, PURPLE)
            check("the heading at the end is out of sight at the top",
                  box is None)
            link, n = pixels(q, page[0], page[1], page[2] - 20, page[3], BROWN)
            seen = len(q.serial())
            q.click((link[0] + link[2]) // 2, link[3] - 3)
            q.mouse_goto(sw - 20, sh // 2)
            time.sleep(1.5)
            box, n = pixels(q, *page, PURPLE)
            check(f"a click on '#end' scrolls to it - the heading shows "
                  f"({box})", box is not None and
                  "[web] went to #end" in q.serial()[seen:] and
                  wait_title(q, geom, "Web: Three") == "Web: Three")

            # --- reload: what the server has now -------------------------
            SERVED["/three.html"] = three(b"Three again")
            q.click(cx + 10 + 2 * step + 14, cy + bar // 2)
            q.mouse_goto(sw - 20, sh // 2)
            got = wait_title(q, geom, "Web: Three again")
            check(f"the reload button fetches the page again ({got!r})",
                  got == "Web: Three again")

            # --- a form ---------------------------------------------------
            q.click(cx + define(SRC, "ADDR_X") + 60, cy + bar // 2)
            q.mouse_goto(sw - 20, sh // 2)
            time.sleep(1)
            typed(q, f"http://10.0.2.2:{port}/form.html")
            q.sendkey("ret")
            got = wait_title(q, geom, "Web: Form")
            time.sleep(1)
            field, _ = pixels(q, *page, FIELD)
            tick, _ = pixels(q, *page, TICK_BG)
            go, _ = pixels(q, *page, GO_BG)
            send, _ = pixels(q, *page, PINK)
            check(f"a form's field, box and buttons are drawn ({got!r}: "
                  f"{field}, {tick}, {go}, {send})",
                  got == "Web: Form" and None not in (field, tick, go, send)
                  and field[2] - field[0] > 100)
            grey = lambda c: (0x60 < c[0] < 0xB0 and abs(c[0] - c[1]) < 12
                              and abs(c[1] - c[2]) < 12)
            before = colours(q, field, grey)
            q.click((field[0] + field[2]) // 2, (field[1] + field[3]) // 2)
            q.mouse_goto(sw - 20, sh // 2)
            time.sleep(1.5)
            after_ = colours(q, field, grey)
            caret = colours(q, field, lambda c: c == (0x20, 0x20, 0x24))
            check(f"the field shows its placeholder until it is clicked; "
                  f"then a caret ({before}, {after_}, {caret} pixels)",
                  before > 10 and after_ == 0 and caret > 5)
            seen = len(q.serial())
            typed(q, "two words")
            q.sendkey("ret")
            got = wait_title(q, geom, "Web: Found")
            sent = [ln for ln in q.serial()[seen:].splitlines()
                    if "form sent" in ln]
            check(f"Enter sends the form: the field, the hidden value and "
                  f"the ticked box, encoded ({got!r}, {sent})",
                  got == "Web: Found" and
                  any(ln.endswith("/find?q=two+words&lang=en&all=on")
                      for ln in sent))

            q.click(cx + define(SRC, "BACK_W") // 2 + 10, cy + bar // 2)
            wait_title(q, geom, "Web: Form")
            time.sleep(1)
            q.click((field[0] + field[2]) // 2, (field[1] + field[3]) // 2)
            time.sleep(0.5)
            typed(q, "abc")
            q.click((tick[0] + tick[2]) // 2, (tick[1] + tick[3]) // 2)
            time.sleep(0.5)
            q.click((go[0] + go[2]) // 2, (go[1] + go[3]) // 2)
            q.mouse_goto(sw - 20, sh // 2)
            got = wait_title(q, geom, "Web: Found plain")
            check(f"a click unticks the box, and the button sends the form "
                  f"without it ({got!r})", got == "Web: Found plain")

            q.click(cx + define(SRC, "BACK_W") // 2 + 10, cy + bar // 2)
            wait_title(q, geom, "Web: Form")
            time.sleep(1)
            q.click((send[0] + send[2]) // 2, (send[1] + send[3]) // 2)
            q.mouse_goto(sw - 20, sh // 2)
            got = wait_title(q, geom, "Web: Sent")
            check(f"a <button> sends its own form - a POST one - with its "
                  f"name and value and the field's preset one ({got!r})",
                  got == "Web: Sent")

            # --- a list, a text of lines, and a POST ----------------------
            q.click(cx + define(SRC, "ADDR_X") + 60, cy + bar // 2)
            q.mouse_goto(sw - 20, sh // 2)
            time.sleep(1)
            typed(q, f"http://10.0.2.2:{port}/post.html")
            q.sendkey("ret")
            got = wait_title(q, geom, "Web: Post form")
            time.sleep(1)
            sel, _ = pixels(q, *page, SELECT_BG)
            area, _ = pixels(q, *page, AREA_BG)
            post, _ = pixels(q, *page, GO_BG)
            check(f"the list, the text box and the button are drawn "
                  f"({got!r}: {sel}, {area}, {post})",
                  got == "Web: Post form" and None not in (sel, area, post)
                  and area[3] - area[1] > 40)
            q.click((sel[0] + sel[2]) // 2, (sel[1] + sel[3]) // 2)
            q.mouse_goto(sw - 20, sh // 2)
            time.sleep(1.5)
            row, _ = pixels(q, page[0], sel[3], page[2], page[3], CHOSEN)
            check(f"a click opens the list under the box, the chosen option "
                  f"(the second) lit ({row})", row is not None and
                  sel[3] < row[1] < sel[3] + 60)
            seen = len(q.serial())
            h = row[3] - row[1] + 1
            q.click((row[0] + row[2]) // 2, row[1] + h + h // 2)
            q.mouse_goto(sw - 20, sh // 2)
            time.sleep(1.5)
            gone, _ = pixels(q, page[0], sel[3], page[2], page[3], CHOSEN)
            check("a click on the third option chooses it, and the list "
                  "closes", "chose 'Large'" in q.serial()[seen:] and
                  gone is None)
            q.click((area[0] + area[2]) // 2, (area[1] + area[3]) // 2)
            time.sleep(0.5)
            q.sendkey("ret")
            typed(q, "second")
            q.click((post[0] + post[2]) // 2, (post[1] + post[3]) // 2)
            q.mouse_goto(sw - 20, sh // 2)
            got = wait_title(q, geom, "Web: Posted")
            sent = [b for p, b in POSTED if p == "/post"]
            check(f"the button POSTs the form: the chosen option's value, "
                  f"the text's lines ending in CR LF ({got!r}, {sent})",
                  got == "Web: Posted" and
                  sent[-1:] == [b"size=l&note=first%0D%0Asecond"])

            # --- cookies: signing in and out -----------------------------
            q.click(cx + define(SRC, "ADDR_X") + 60, cy + bar // 2)
            q.mouse_goto(sw - 20, sh // 2)
            time.sleep(1)
            typed(q, f"http://10.0.2.2:{port}/login.html")
            q.sendkey("ret")
            wait_title(q, geom, "Web: Login")
            time.sleep(1)
            field, _ = pixels(q, *page, FIELD)
            q.click((field[0] + field[2]) // 2, (field[1] + field[3]) // 2)
            time.sleep(0.5)
            typed(q, "daniu")
            q.sendkey("ret")
            got = wait_title(q, geom, "Web: Signed in")
            home = [c for p, c in COOKIES if p == "/home"]
            pic = [c for p, c in COOKIES if p == "/avatar.png"]
            check(f"the cookie the login set with its redirect comes back "
                  f"with the next page, and with its picture - and not the "
                  f"cookies for another path or domain ({got!r}, {home}, "
                  f"{pic})", got == "Web: Signed in" and
                  home[-1:] == ["session=abc123"] and
                  pic[-1:] == ["session=abc123"])
            q.click(cx + define(SRC, "ADDR_X") + 60, cy + bar // 2)
            q.mouse_goto(sw - 20, sh // 2)
            time.sleep(1)
            typed(q, f"http://10.0.2.2:{port}/logout")
            q.sendkey("ret")
            got = wait_title(q, geom, "Web: Signed out")
            home = [c for p, c in COOKIES if p == "/home"]
            check(f"signing out removes it: the next page comes without "
                  f"one ({got!r}, {home[-1:]})", got == "Web: Signed out"
                  and home[-1:] == [None])

            # --- selecting, copying, Tab and pasting ---------------------
            q.click(cx + define(SRC, "ADDR_X") + 60, cy + bar // 2)
            q.mouse_goto(sw - 20, sh // 2)
            time.sleep(1)
            typed(q, f"http://10.0.2.2:{port}/copy.html")
            q.sendkey("ret")
            wait_title(q, geom, "Web: Copy")
            time.sleep(1)
            words, _ = pixels(q, *page, VIOLET)
            mid = (words[1] + words[3]) // 2
            seen = len(q.serial())
            q.mouse_goto(words[0] - 4, mid)
            q.mouse_button(True)
            time.sleep(0.3)
            q.mouse_goto((words[0] + words[2]) // 2, mid)
            time.sleep(0.3)
            q.mouse_goto(words[2] + 4, mid)
            time.sleep(0.3)
            q.mouse_button(False)
            time.sleep(1.5)
            sel_bg = (0xB4, 0xD5, 0xFE)
            lit, n = pixels(q, *page, sel_bg)
            check(f"a drag across words lights them ({lit}) and copies "
                  f"them", lit is not None and lit[0] <= words[0] and
                  lit[2] >= words[2] and
                  "[web] copied: copy these words" in q.serial()[seen:])
            field, _ = pixels(q, *page, FIELD)
            q.click((field[0] + field[2]) // 2, (field[1] + field[3]) // 2)
            q.mouse_goto(sw - 20, sh // 2)
            time.sleep(0.5)
            typed(q, "a")
            q.sendkey("tab")
            time.sleep(0.3)
            q.sendkey("shift-insert")
            time.sleep(0.3)
            q.sendkey("ret")
            got = wait_title(q, geom, "Web: Pasted")
            check(f"Tab goes to the next field, and Shift+Insert pastes "
                  f"what was copied - through the kernel's clipboard "
                  f"({got!r})", got == "Web: Pasted")
        finally:
            q.close()
    finally:
        stop()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
