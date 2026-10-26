#!/usr/bin/env python3
"""ThreadOS smoke test: forms, cookies and the clipboard in the browser.

Each checked from outside, against the server on the host, which answers
only to exactly what a browser must send:

  a field takes the keys after a click; Enter or a button sends the
  form - its fields, its hidden values, its ticked boxes, the pressed
  button and not the others; a <button> in a POST form posts

  a <select> opens a list to choose from; a <textarea> takes lines;
  POST sends them, the lines ending in CR LF

  a sign-in's cookie, set with a redirect, comes back with the next page
  and its picture - not those for another path or domain - and signing
  out removes it

  a drag selects and copies words; Tab moves between fields and
  Shift+Insert pastes into one, through the kernel's clipboard

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webtest import *        # noqa: F401,F403
from webtest import check, define, run, typed, visit


def forms(e):
    """Forms of every kind, POST, cookies, selecting and pasting"""
    q, port, sw, sh, w0, h0 = e.q, e.port, e.sw, e.sh, e.w0, e.h0
    geom, cx, cy, bar, page, step = e.geom, e.cx, e.cy, e.bar, e.page, e.step
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

    # --- a form on a windows-1250 page ------------------------------------
    seen = len(q.serial())
    visit(e, "/plform.html", "Web: Formularz")
    button, _ = pixels(q, *page, GO_BG)
    q.click((button[0] + button[2]) // 2, (button[1] + button[3]) // 2)
    q.mouse_goto(sw - 20, sh // 2)
    got = wait_title(q, geom, "Web: Sent in 1250")
    sent = re.findall(r"\[web\] form sent: \S*(/plform\?[^\r\n]*)",
                      q.serial()[seen:])
    check(f"a form on a windows-1250 page sends in windows-1250, a "
          f"snowman as &#9731; ({sent[-1:]}, {got!r})",
          sent[-1:] == [PLFORM_SENT] and got == "Web: Sent in 1250")


def main():
    return run("forms, cookies and the clipboard in the browser", [forms])


sys.exit(main())
