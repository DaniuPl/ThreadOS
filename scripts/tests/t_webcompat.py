#!/usr/bin/env python3
"""ThreadOS smoke test: what real sites need of the browser, found by
showing them (Hacker News, Wikipedia, the Python documentation).

  addresses with "." and ".." in them are put right before they are
  fetched (RFC 3986, 5.2.4): a style sheet at "../css/style.css" and a
  picture at "./../img/p.png" come from where they are - the server
  here, like many, has no files under such names

  <center> centres the table in it, not only text; and a page with no
  <!doctype html> is in quirks mode, where a table does not take its
  text alignment from around it: a <center>ed table's text starts at the
  left of its cell. With the doctype it is centred.

  an address longer than a program argument holds - Wikipedia's style
  sheets have them - is fetched whole

  JavaScript's URL class takes addresses apart and puts them together
  against a base as Node.js does, address for address

Run on its own or through scripts/run_tests.py.
"""

import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webtest import *        # noqa: F401,F403
from webtest import check, run, visit

ORANGE = (0xE0, 0x70, 0x10)
LONG = "/w/load.php?" + "&".join(f"modules=m{i}.styles" for i in range(30)) + "&only=styles"

DOTS = (b"<!doctype html><html><head><title>Dots</title>"
        b"<link rel=stylesheet href='../css/style.css'></head><body>"
        b"<p class=box>styled</p><p><img src='./../img/p.png'></p></body></html>")
STYLE = b".box{background:#e07010;width:150px;height:40px}"

URLS = [("../a/./b?x=1#h", "http://h.test:8/p/q/r"),
        ("//other.test/x", "https://h.test/p"),
        ("?q=a%20b", "http://h.test/p/r?old=1"),
        ("#top", "http://h.test/p/r?q=1"),
        ("/abs/../c/", "http://h.test/p/r"),
        ("../../../../up", "http://h.test/a/b"),
        ("https://User:Pw@Example.COM:8443/P?a=1&b=2#f", None),
        ("rel", "http://h.test/dir/"),
        ("", "http://h.test/p?q#f")]
PROPS = ["href", "protocol", "host", "hostname", "port", "pathname", "search",
         "hash", "origin", "username", "password"]


def node_urls():
    code = ("const cases = %s, props = %s;\n"
            "console.log(JSON.stringify(cases.map(([r, b]) => {"
            " const u = b === null ? new URL(r) : new URL(r, b);"
            " return props.map(p => u[p]); })));" % (json.dumps(URLS), json.dumps(PROPS)))
    try:
        out = subprocess.run(["node", "-e", code], capture_output=True, text=True,
                             timeout=30)
        return json.loads(out.stdout) if out.returncode == 0 else None
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None


def page_urls():
    script = ("const cases = %s, props = %s; const out = cases.map(([r, b]) => {"
              " try { const u = b === null ? new URL(r) : new URL(r, b);"
              " return props.map(p => u[p]); } catch (e) { return String(e); } });"
              " console.log('urls ' + JSON.stringify(out));"
              % (json.dumps(URLS), json.dumps(PROPS)))
    return ("<html><head><title>URLs</title></head><body><script>" + script +
            "</script></body></html>").encode()


def table_page(doctype):
    return ((b"<!doctype html>" if doctype else b"") +
            b"<html><head><title>Table</title></head><body style='margin:0'><center>"
            b"<table cellpadding=0 cellspacing=0><tr><td style='width:400px;"
            b"background:#fff0d0;color:#802080'>left or middle</td></tr></table>"
            b"</center></body></html>")


def compat(e):
    q, page = e.q, e.page
    SERVED["/site/pages/dots.html"] = DOTS
    SERVED["/site/css/style.css"] = STYLE
    SERVED["/site/img/p.png"] = make_png(64, 48)
    SERVED["/quirks.html"] = table_page(False)
    SERVED["/standard.html"] = table_page(True)
    SERVED["/long.html"] = (b"<!doctype html><html><head><title>Long</title>"
                            b"<link rel=stylesheet href='" + LONG.replace("&", "&amp;").encode() +
                            b"'></head><body><p class=box>styled</p></body></html>")
    SERVED[LONG] = STYLE
    SERVED["/urls.html"] = page_urls()

    seen = len(q.serial())
    visit(e, "/site/pages/dots.html", "Web: Dots")
    time.sleep(1)
    box, _ = pixels(q, *page, ORANGE)
    red, _ = pixels(q, *page, (255, 0, 0))
    check(f"'../css/style.css' and './../img/p.png' come from where they are "
          f"({box}, {red})", box is not None and box[2] - box[0] + 1 == 150 and
          red is not None and "/site/pages/../" not in q.serial()[seen:])

    def text_x(path, title):
        visit(e, path, title)
        time.sleep(1)
        cell, _ = pixels(q, *page, (0xFF, 0xF0, 0xD0))
        ink, _ = pixels(q, *page, (0x80, 0x20, 0x80))
        return cell, ink
    cell, ink = text_x("/quirks.html", "Web: Table")
    mid = (page[0] + page[2]) / 2
    check(f"<center> centres the table itself ({cell})",
          cell and abs((cell[0] + cell[2]) / 2 - mid) < 4)
    check(f"quirks mode: a <center>ed table's text starts at the left of its "
          f"cell ({cell}, {ink})", cell and ink and ink[0] - cell[0] < 10)
    cell, ink = text_x("/standard.html", "Web: Table")
    check(f"with <!doctype html> it is centred in it ({cell}, {ink})",
          cell and ink and abs((ink[0] + ink[2]) / 2 - (cell[0] + cell[2]) / 2) < 4
          and abs((cell[0] + cell[2]) / 2 - mid) < 4)

    seen = len(q.serial())
    visit(e, "/long.html", "Web: Long")
    time.sleep(1)
    box, _ = pixels(q, *page, ORANGE)
    check(f"a style sheet at an address of {len(LONG)} characters is fetched "
          f"whole ({box})", box is not None and box[2] - box[0] + 1 == 150)

    want = node_urls()
    seen = len(q.serial())
    visit(e, "/urls.html", "Web: URLs")
    end = time.time() + 10
    got = None
    while time.time() < end and got is None:
        for ln in q.serial()[seen:].splitlines():
            if ln.startswith("[js] urls "):
                got = json.loads(ln[len("[js] urls "):])
        time.sleep(0.3)
    if want is None:
        check("Node.js is here to compare URL with (it is not: skipped)", True)
        return
    wrong = [(URLS[i], PROPS[k], g[k], w[k]) for i, (g, w) in enumerate(zip(got or [], want))
             for k in range(len(PROPS)) if not isinstance(g, list) or g[k] != w[k]]
    check(f"URL takes {len(URLS)} addresses apart as Node.js does, "
          f"{len(PROPS)} parts each (wrong: {wrong[:3]})",
          got is not None and len(got) == len(want) and not wrong)


def main():
    return run("what real sites need", [compat])


sys.exit(main())
