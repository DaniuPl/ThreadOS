#!/usr/bin/env python3
"""ThreadOS smoke test: the browser's tools.

  Ctrl+F finds and lights every match, walks them and scrolls to them;
  a .txt file is shown as text

  a link to a file that is not a page saves it in the home directory,
  never over another, and the kernel's 'cat' reads it whole

  the star keeps bookmarks; they and the history are on the start page,
  and outlive the browser

  a slow page can be stopped while it comes; the right button opens a
  link in a new window; the page zooms to 150% and back

  a page's characters are decoded as its <meta>, its server or the
  system's default say - held to Python's own codecs

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webtest import *        # noqa: F401,F403
from webtest import check, define, run, typed


def tools(e):
    """Find, plain text, downloads, bookmarks, stop, new windows, zoom, characters"""
    q, port, sw, sh, w0, h0 = e.q, e.port, e.sw, e.sh, e.w0, e.h0
    geom, cx, cy, bar, page, step = e.geom, e.cx, e.cy, e.bar, e.page, e.step
    # --- finding on the page -------------------------------------
    def go(path, title):
        q.click(cx + define(SRC, "ADDR_X") + 60, cy + bar // 2)
        q.mouse_goto(sw - 20, sh // 2)
        time.sleep(1)
        typed(q, f"http://10.0.2.2:{port}{path}")
        q.sendkey("ret")
        got = wait_title(q, geom, title)
        time.sleep(1)
        return got

    def found(want, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            lines = [ln for ln in q.serial()[seen:].splitlines()
                     if "[web] find" in ln]
            if lines and lines[-1].endswith(want):
                return lines[-1]
            time.sleep(0.3)
        return lines[-1] if lines else ""

    go("/find.html", "Web: Find")
    seen = len(q.serial())
    q.sendkey("ctrl-f")
    time.sleep(0.5)
    typed(q, "apple")
    said = found("1 of 3")
    time.sleep(1)
    now, _ = pixels(q, *page, ORANGE)
    check(f"Ctrl+F and a word: three matches, whatever their case, "
          f"the first lit ({said!r}, {now})",
          said.endswith("'apple': 1 of 3") and now is not None)
    q.sendkey("ret")
    q.sendkey("ret")
    said = found("3 of 3")
    time.sleep(1)
    now, _ = pixels(q, *page, ORANGE)
    check(f"Enter walks to the third, far down the page - brought "
          f"into sight ({said!r}, {now})",
          said.endswith("3 of 3") and now is not None)
    q.sendkey("esc")
    time.sleep(1)
    now, _ = pixels(q, *page, ORANGE)
    check("Escape closes the search, and the light goes",
          now is None)

    # --- plain text is shown as text -----------------------------
    got = go("/notes.txt", "Web: notes.txt")
    seen = len(q.serial())
    q.sendkey("ctrl-f")
    time.sleep(0.5)
    typed(q, "<b>")
    said = found("1 of 1")
    q.sendkey("esc")
    check(f"a .txt file is shown as it is: its '<b>' is letters on "
          f"the page, not markup ({got!r}, {said!r})",
          got == "Web: notes.txt" and said.endswith("1 of 1"))

    # --- downloading ---------------------------------------------
    go("/dl.html", "Web: Downloads")
    link, _ = pixels(q, *page, BROWN)
    saved = []
    for k in range(2):
        seen = len(q.serial())
        q.click((link[0] + link[2]) // 2, link[3] - 3)
        q.mouse_goto(sw - 20, sh // 2)
        end = time.time() + 20
        m = None
        while time.time() < end and not m:
            m = re.search(r"\[web\] saved (\S+) \((\d+) bytes\)",
                          q.serial()[seen:])
            time.sleep(0.3)
        saved.append((m.group(1), int(m.group(2))) if m else None)
    title = q.window_title(*geom)
    check(f"a link to a file that is not a page saves it, with its "
          f"size, and the page stays ({saved}, {title!r})",
          None not in saved and title == "Web: Downloads" and
          all(n == len(REPORT) for _, n in saved) and
          saved[0][0].endswith("/report.dat") and
          saved[1][0].endswith("/report-1.dat"))
    if saved[0]:
        q.click_desktop()
        time.sleep(0.5)
        out = q.command("cat " + saved[0][0], "line three",
                        timeout=15)
        check(f"and the kernel's own 'cat' reads it whole from the "
              f"disk ({out.count('downloaded-ok')} lines)",
              out.count("downloaded-ok") == 3)

    # --- bookmarks and history -----------------------------------
    q.click(cx + w0 // 2, cy + h0 // 2)        # the keys back
    go("/two.html", "Web: Page two")
    star_x = cx + w0 - 10 - define(SRC, "STAR_W")
    star = (star_x, cy + 8, star_x + define(SRC, "STAR_W") - 8, cy + 32)
    accent = theme("dark")["accent"]

    def star_lit():
        return colours(q, star, lambda c: near(c, accent, 10))

    unlit = star_lit()
    seen = len(q.serial())
    q.click(star_x + 10, cy + bar // 2)
    q.mouse_goto(sw - 20, sh // 2)
    time.sleep(1.5)
    lit = star_lit()
    check(f"the star keeps the page among the bookmarks, and fills "
          f"({unlit} -> {lit} pixels)", unlit == 0 and lit > 10 and
          "[web] bookmarked http://10.0.2.2:%d/two.html" % port
          in q.serial()[seen:])

    def find_page_two():
        """Ctrl+F for "page two": what the find said"""
        seen0 = len(q.serial())
        q.sendkey("ctrl-f")
        time.sleep(0.5)
        typed(q, "page two")
        end = time.time() + 10
        said = ""
        while time.time() < end and not said:
            said = next((ln for ln in reversed(
                q.serial()[seen0:].splitlines()) if "[web] find" in ln),
                "")
            time.sleep(0.3)
        q.sendkey("esc")
        return said

    q.click(cx + define(SRC, "ADDR_X") + 60, cy + bar // 2)
    q.mouse_goto(sw - 20, sh // 2)
    time.sleep(1)
    typed(q, "about:start")
    q.sendkey("ret")
    got = wait_title(q, geom, "Web: Start")
    time.sleep(1)
    said = find_page_two()
    check(f"the start page lists it twice: among the bookmarks and "
          f"the pages seen last ({got!r}, {said!r})",
          got == "Web: Start" and said.endswith("1 of 2"))

    # They outlive the browser: closed, and opened again
    q.sendkey("alt-f4")
    time.sleep(2)
    q.n_win -= 1
    q.launch_dock("gweb", w=sw, h=sh)
    got = wait_title(q, geom, "Web: Start")
    time.sleep(1)
    said = find_page_two()
    check(f"opened again, the browser still has both - they are on "
          f"the disk ({got!r}, {said!r})",
          got == "Web: Start" and said.endswith("1 of 2"))
    mark, _ = pixels(q, *page, (0x1A, 0x55, 0xC0))
    q.click((mark[0] + mark[2]) // 2, mark[3] - 3)
    q.mouse_goto(sw - 20, sh // 2)
    got = wait_title(q, geom, "Web: Page two")
    time.sleep(1)
    lit = star_lit()
    check(f"a click on the bookmark opens the page, its star full "
          f"({got!r}, {lit})", got == "Web: Page two" and lit > 10)
    seen = len(q.serial())
    q.click(star_x + 10, cy + bar // 2)
    q.mouse_goto(sw - 20, sh // 2)
    time.sleep(1.5)
    check("and the star again takes it out", star_lit() == 0 and
          "[web] bookmark removed" in q.serial()[seen:])

    # --- a slow page, stopped ------------------------------------
    # Twelve pieces a second apart: the window must answer while
    # they come, and the cross must end it
    ROUTES["/slow"] = lambda h, b: (
        200, [("Content-Type", "text/html")],
        [b"<html><head><title>Slow</title></head><body>"] +
        [b"<p>" + b"x" * 2000 + b"</p>"] * 11)
    seen = len(q.serial())
    q.click(cx + define(SRC, "ADDR_X") + 60, cy + bar // 2)
    q.mouse_goto(sw - 20, sh // 2)
    time.sleep(1)
    typed(q, f"http://10.0.2.2:{port}/slow")
    q.sendkey("ret")
    time.sleep(5)
    q.click(cx + 10 + 2 * step + 14, cy + bar // 2)
    q.mouse_goto(sw - 20, sh // 2)
    m = None
    end = time.time() + 10
    while time.time() < end and not m:
        m = re.search(r"\[web\] stopped loading \S+/slow after "
                      r"(\d+) bytes", q.serial()[seen:])
        time.sleep(0.3)
    time.sleep(1)
    title = q.window_title(*geom)
    check(f"the cross stops a page still coming, part of it in "
          f"({m.group(1) if m else None} bytes), and the page "
          f"before stays ({title!r})",
          m is not None and 4000 <= int(m.group(1)) < 22000 and
          title == "Web: Page two")

    # --- a link into a new window --------------------------------
    go("/page.html", "Web: Test page")
    link, _ = pixels(q, page[0], page[1], page[2] - 20, page[3], LINK)
    q.mouse_goto((link[0] + link[2]) // 2, link[3] - 2)
    time.sleep(0.3)
    q.mouse_right_click()
    q.n_win += 1
    nx, ny = cascade(3, w0 + 2, h0 + TITLE_H + 2, w=sw, h=sh)
    got = wait_title(q, (nx, ny, w0 + 2), "Web: Page two")
    # (the new one lies over the first's title: a click on the
    # strip of it still showing brings the first forward)
    q.click(geom[0] + w0 // 2, geom[1] + 4)
    q.mouse_goto(sw - 20, sh // 2)
    time.sleep(1)
    first = q.window_title(*geom)
    check(f"the right button on a link opens it in a new window "
          f"({got!r}), and the first stays where it was "
          f"({first!r})",
          got == "Web: Page two" and first == "Web: Test page")

    # --- zoom ----------------------------------------------------
    # (the first window is in front again, on the test page)
    zx = cx + w0 - define(SRC, "ZOOM_W")
    zb = define(SRC, "ZOOM_BTN")
    zy = cy + h0 - define(SRC, "STATUS_H") // 2
    red0, _ = pixels(q, *page, RED)
    seen = len(q.serial())
    for _ in range(3):                     # 110, 125, 150
        q.click(cx + w0 - zb // 2, zy)
        time.sleep(0.8)
    q.mouse_goto(sw - 20, sh // 2)
    time.sleep(1.5)
    red1, _ = pixels(q, *page, RED)
    w_0 = red0[2] - red0[0] + 1 if red0 else 0
    w_1 = red1[2] - red1[0] + 1 if red1 else 0
    check(f"three steps of + zoom the page to 150%: the heading is "
          f"half as wide again ({w_0} -> {w_1} px)",
          "[web] zoom 150%" in q.serial()[seen:] and w_0 > 0 and
          abs(w_1 - w_0 * 1.5) <= w_0 * 0.04)
    link, _ = pixels(q, page[0], page[1], page[2] - 20, page[3], LINK)
    q.click((link[0] + link[2]) // 2, link[3] - 2)
    q.mouse_goto(sw - 20, sh // 2)
    got = wait_title(q, geom, "Web: Page two")
    check(f"a click on the link where the zoom drew it follows it "
          f"({got!r})", got == "Web: Page two")
    q.click(cx + define(SRC, "BACK_W") // 2 + 10, cy + bar // 2)
    wait_title(q, geom, "Web: Test page")
    q.click(zx + define(SRC, "ZOOM_W") // 2, zy)
    q.mouse_goto(sw - 20, sh // 2)
    time.sleep(1.5)
    red2, _ = pixels(q, *page, RED)
    check(f"a click on the percentage brings it back to 100% "
          f"({red2} vs {red0})", red2 == red0)

    # --- characters ----------------------------------------------
    for path, want, how in (
            ("/pl-meta.html", PL, "a <meta charset=windows-1250>"),
            ("/pl-http.html", PL, "the server's charset=ISO-8859-2"),
            ("/latin.html", LATIN, "nothing: the default, "
                                   "windows-1252")):
        seen = len(q.serial())
        q.click(cx + define(SRC, "ADDR_X") + 60, cy + bar // 2)
        q.mouse_goto(sw - 20, sh // 2)
        time.sleep(1)
        typed(q, f"http://10.0.2.2:{port}{path}")
        q.sendkey("ret")
        end = time.time() + 20
        got = []
        while time.time() < end and not got:
            # (to the line's end by \r and \n only: splitlines() would
            # also break at \x85, a byte of UTF-8's "ą" read as latin1)
            got = re.findall(r"\[web\] title: ([^\r\n]*)",
                             q.serial()[seen:])
            time.sleep(0.3)
        check(f"a page's characters as {how} says "
              f"({got[-1:]!r})", got[-1:] == [on_serial(want)])
    # Plain text in windows-1250: shown in UTF-8, as a copy of it
    # shows
    go("/pl.txt", "Web: pl.txt")
    words, _ = pixels(q, *page, (0, 0, 0))
    seen = len(q.serial())
    if words:
        mid = (words[1] + words[3]) // 2
        q.mouse_goto(words[0] - 4, mid)
        q.mouse_button(True)
        time.sleep(0.3)
        q.mouse_goto(words[2] + 4, mid)
        time.sleep(0.3)
        q.mouse_button(False)
        q.mouse_goto(sw - 20, sh // 2)
        time.sleep(1.5)
    check("a .txt file in windows-1250 reads as the words it holds",
          "[web] copied: " + on_serial(PL) in q.serial()[seen:])


    # --- tabs -------------------------------------------------------------
    tab_h, tab_max = define(SRC, "TAB_H"), define(SRC, "TAB_MAX")
    strip = cy + bar + tab_h // 2
    page2 = (page[0], page[1] + tab_h, page[2], page[3])

    def tab_x(i, n):
        """The middle of tab i of n, on the strip"""
        tw = min(tab_max, (w0 - 16) // n)
        return cx + 8 + i * tw + tw // 2, cx + 8 + i * tw + tw - 15

    def tabs_said(want, timeout=15):
        end = time.time() + timeout
        said = []
        while time.time() < end:
            said = re.findall(r"\[web\] tabs: (\d+), at (\d+)",
                              q.serial()[seen:])
            if said and said[-1] == want:
                break
            time.sleep(0.3)
        return said[-1] if said else None

    first = q.window_title(*geom)
    seen = len(q.serial())
    q.click(cx + 10 + 3 * step + 14, cy + bar // 2)          # the plus
    q.mouse_goto(sw - 20, sh // 2)
    got = wait_title(q, geom, "Web: Start")
    check(f"the plus opens a new tab on the start page ({got!r}, "
          f"{tabs_said(('2', '2'))})", got == "Web: Start" and
          tabs_said(("2", "2")) == ("2", "2"))
    q.click(cx + define(SRC, "ADDR_X") + 60, cy + bar // 2)
    q.mouse_goto(sw - 20, sh // 2)
    time.sleep(1)
    typed(q, f"http://10.0.2.2:{port}/page.html")
    q.sendkey("ret")
    wait_title(q, geom, "Web: Test page")
    time.sleep(1)
    link, _ = pixels(q, page2[0], page2[1], page2[2] - 20, page2[3], LINK)
    seen = len(q.serial())
    q.mouse_goto((link[0] + link[2]) // 2, link[3] - 2)
    time.sleep(0.3)
    q.mouse_button(True, "middle")
    q.mouse_button(False, "middle")
    q.mouse_goto(sw - 20, sh // 2)
    said = tabs_said(("3", "2"))
    time.sleep(1)
    check(f"the middle button opens a link in a tab behind this one "
          f"({said}, {q.window_title(*geom)!r})", said == ("3", "2") and
          q.window_title(*geom) == "Web: Test page")
    q.click(tab_x(2, 3)[0], strip)
    q.mouse_goto(sw - 20, sh // 2)
    got = wait_title(q, geom, "Web: Page two")
    check(f"a click on that tab shows its page ({got!r})",
          got == "Web: Page two")
    q.click(tab_x(0, 3)[0], strip)
    q.mouse_goto(sw - 20, sh // 2)
    got = wait_title(q, geom, first)
    check(f"and the first tab still has its own ({got!r})", got == first)
    seen = len(q.serial())
    q.click(tab_x(2, 3)[1], strip)
    q.mouse_goto(sw - 20, sh // 2)
    said3 = tabs_said(("2", "1"))
    q.click(tab_x(1, 2)[1], strip)
    q.mouse_goto(sw - 20, sh // 2)
    said2 = tabs_said(("1", "1"))
    time.sleep(1)
    check(f"the crosses close tabs, down to one, and the strip goes "
          f"({said3}, {said2}, {q.window_title(*geom)!r})",
          said3 == ("2", "1") and said2 == ("1", "1") and
          q.window_title(*geom) == first)


def main():
    return run("the browser's tools", [tools])


sys.exit(main())
