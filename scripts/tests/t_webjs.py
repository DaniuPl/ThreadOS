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

  a page that is an app: keys typed into its field reach its input
  listener, and one its keydown listener keeps never gets in; leaving
  the field is a change; a box ticked and a choice made are changes too;
  its submit handler keeps the form and sends the fields itself, as
  JSON with fetch() - the server's own account says what came, and the
  answer it gave is on the window; an XMLHttpRequest reads a 404 as an
  answer; a request to another site is refused before anything is sent;
  and form.submit() sends the form as the browser would

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


# A page that is an app. Its parts are found on the screen by their
# colours: the field by its own, the box and the list just after a
# marker of their own.
APP = b"""<html><head><title>App</title></head><body>
<form id=f action=/never method=post>
<p><input id=name name=name style='background:#fff0d0;width:200px'></p>
<p><span style='background:#40c040'>MMMM</span><input type=checkbox
 id=agree name=agree></p>
<p><span style='background:#4040c0'>MMMM</span><select id=size name=size>
<option value=s>Small<option value=l>Large</select></p>
<p><input type=submit value=Send style='background:#e6e6ea'>
<input type=button id=plain value=Plain style='background:#d0f0d0'></p>
</form>
<p id=echo style='color:#aa0066'>nothing typed</p>
<script>
const log = m => console.log(m);
const $ = id => document.getElementById(id);
const name = $('name');
name.addEventListener('input', e => {
  $('echo').textContent = 'typed ' + e.target.value;
});
name.addEventListener('keydown', e => {
  if (e.key === 'x') { e.preventDefault(); log('kept the x'); }
});
name.addEventListener('change', () => log('change: name=' + name.value));
$('agree').addEventListener('change', e => log('change: agree=' + e.target.checked));
$('size').onchange = e => log('change: size=' + e.target.value);
document.addEventListener('keydown', e => {
  if (e.key === 'ArrowDown') log('the page heard ArrowDown ' + e.keyCode);
});
$('plain').addEventListener('click', () => $('f').submit());
$('f').addEventListener('submit', async e => {
  e.preventDefault();
  log('form: ' + new FormData($('f')));
  const r = await fetch('/api/echo', {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ name: name.value, agree: $('agree').checked })
  });
  const j = await r.json();
  document.title = 'Got ' + j.reply;
  log('fetch: ' + r.status + ' ' + r.ok + ' ' + r.headers.get('Content-Type'));
  const x = new XMLHttpRequest();
  x.open('GET', '/api/missing');
  x.onload = () => log('xhr: ' + x.status + ' ' + x.responseText);
  x.send();
  fetch('http://example.com/data').then(() => log('cross: fetched'),
    err => log('cross: refused ' + (err instanceof TypeError)));
});
</script></body></html>"""

API_SEEN = []           # what /api/echo was sent: its type and body


def r_echo(h, body):
    import json
    API_SEEN.append((h.command, h.headers.get("Content-Type"), body))
    try:
        sent = json.loads(body)
        reply = sent["name"].upper() + ("+" if sent["agree"] is True else "-")
    except (ValueError, KeyError, AttributeError):
        reply = "nonsense"
    return 200, [("Content-Type", "application/json")], \
        json.dumps({"reply": reply}).encode()


def r_missing(h, body):
    return 404, [("Content-Type", "text/plain")], b"nothing here"


def apps(e):
    q, sw, sh = e.q, e.sw, e.sh
    geom, page = e.geom, e.page
    away = (sw - 20, sh // 2)
    SERVED["/app.html"] = APP
    SERVED["POST /never name=daniu&agree=on&size=l"] = \
        b"<html><head><title>Sent plainly</title></head></html>"
    ROUTES.update({"/api/echo": r_echo, "/api/missing": r_missing})

    def said_since(at, text, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            if text in q.serial()[at:]:
                return True
            time.sleep(0.3)
        return False

    def click_box(colour, dx=0):
        box, _ = pixels(q, *page, colour)
        if box:
            q.click(box[2] + dx if dx else (box[0] + box[2]) // 2,
                    (box[1] + box[3]) // 2)
            q.mouse_goto(*away)
            time.sleep(1)
        return box

    got = visit(e, "/app.html", "Web: App")
    check(f"the app page opens ({got!r})", got == "Web: App")

    # Typing: input on every key, and the x the page keeps; leaving the
    # field is the change - once, with all of it
    seen = len(q.serial())
    field = click_box((0xFF, 0xF0, 0xD0))
    typed(q, "danxiu")
    kept = said_since(seen, "[js] kept the x")
    time.sleep(1)
    typing = "change:" not in q.serial()[seen:]
    q.click((page[0] + page[2]) // 2, page[3] - 30)
    q.mouse_goto(*away)
    changed = said_since(seen, "[js] change: name=daniu\n")
    once = q.serial()[seen:].count("change: name") == 1
    q.sendkey("ctrl-f")
    time.sleep(0.5)
    typed(q, "typed daniu")
    found = said_since(seen, "[web] find 'typed daniu': 1 of 1")
    q.sendkey("esc")
    time.sleep(0.5)
    check(f"keys typed into a field reach its input listener, and the x "
          f"its keydown listener kept is not in it ({field})",
          field is not None and kept and found)
    check(f"leaving the field is a change, once, with what it holds "
          f"(none while typing: {typing})", typing and changed and once)

    # A key outside the field is the page's
    seen = len(q.serial())
    q.sendkey("down")
    check("a key outside reaches the document's listener, with its keyCode",
          said_since(seen, "[js] the page heard ArrowDown 40"))

    seen = len(q.serial())
    tick = click_box((0x40, 0xC0, 0x40), dx=9)
    ticked = said_since(seen, "[js] change: agree=true")
    menu = click_box((0x40, 0x40, 0xC0), dx=30)
    q.sendkey("down")
    time.sleep(0.3)
    q.sendkey("ret")
    chose = said_since(seen, "[js] change: size=l")
    check(f"a box ticked and a choice made in a list are changes "
          f"({tick}, {menu})", ticked and chose)

    # The submit handler sends the fields itself
    seen = len(q.serial())
    n_posted = len(POSTED)
    send = click_box((0xE6, 0xE6, 0xEA))
    got = wait_title(q, geom, "Web: Got DANIU+", timeout=20)
    said = q.serial()[seen:]
    check(f"its submit handler keeps the form: the page is not left "
          f"({send}, {got!r})", "[web] the page kept its form" in said and
          not any(p == "/never" for p, _ in POSTED[n_posted:]))
    check("FormData holds the form's fields as the form would send them",
          "[js] form: name=daniu&agree=on&size=l" in said)
    check(f"fetch() POSTs JSON: the server got it, typed so, and the "
          f"answer it gave names the window ({API_SEEN[-1:]}, {got!r})",
          API_SEEN[-1:] == [("POST", "application/json",
                             b'{"name":"daniu","agree":true}')] and
          got == "Web: Got DANIU+")
    check("the response says its status and type",
          said_since(seen, "[js] fetch: 200 true application/json"))
    check("an XMLHttpRequest reads a 404 as an answer, with its body",
          said_since(seen, "[js] xhr: 404 nothing here"))
    check("a request to another site is refused - a TypeError - before "
          "anything is sent", said_since(seen, "[js] cross: refused true")
          and "[js] request refused: http://example.com/data" in
          q.serial()[seen:] and "example.com (" not in q.serial()[seen:])

    # form.submit(): sent as the browser would, the buttons left out
    plain = click_box((0xD0, 0xF0, 0xD0))
    got = wait_title(q, geom, "Web: Sent plainly", timeout=20)
    check(f"form.submit() sends the form as the browser would ({plain}, "
          f"{got!r})", got == "Web: Sent plainly")


def main():
    return run("JavaScript in the browser", [scripts, apps])


sys.exit(main())
