#!/usr/bin/env python3
"""ThreadOS smoke test: the browser's menu and its own pages.

  the toolbar's menu opens and its items open the browser's pages:
  History, Cookies, Settings

  the history lists the pages seen, and its "Clear the history" empties
  it

  the cookies page lists what a site set, and "Remove all cookies"
  removes it - the site then gets no cookie at all, by its own account

  in the settings, unticking JavaScript and saving keeps the next page's
  scripts from running; ticking it again brings them back

  a page from the network cannot lead to those pages: a link to
  about:history?clear=all is refused, and the history stays

  Ctrl and a click on a link open it in a tab behind this one; with
  Shift as well, in a tab in front; the middle button and Ctrl alike
  refuse a page's link to the browser's own pages

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webtest import *        # noqa: F401,F403
from webtest import check, define, run, typed, visit, wait_title


def ui(e):
    q, port, sw, sh, w0, h0 = e.q, e.port, e.sw, e.sh, e.w0, e.h0
    geom, cx, cy, bar, page, step = e.geom, e.cx, e.cy, e.bar, e.page, e.step
    tab_h = define(SRC, "TAB_H")
    page2 = (page[0], page[1] + tab_h, page[2], page[3])   # under the tabs
    away = (sw - 20, sh // 2)

    def menu(i):
        """Opens the menu and picks its item i"""
        q.click(cx + w0 - 10 - define(SRC, "MENU_W") // 2, cy + bar // 2)
        q.mouse_goto(*away)
        time.sleep(1)
        row = define(SRC, "MENU_ROW")
        q.click(cx + w0 - 10 - define(SRC, "MENU_PANEL_W") + 60,
                cy + bar - 2 + 4 + i * row + row // 2)
        q.mouse_goto(*away)

    def find(text):
        """Ctrl+F for text (lower case: the find ignores case): what the
        find said last"""
        seen0 = len(q.serial())
        q.sendkey("ctrl-f")
        time.sleep(0.5)
        typed(q, text)
        end = time.time() + 10
        said = ""
        while time.time() < end:
            lines = [ln for ln in q.serial()[seen0:].splitlines()
                     if ln.startswith(f"[web] find '{text}'")]
            said = lines[-1] if lines else ""
            if said:
                break
            time.sleep(0.3)
        q.sendkey("esc")
        time.sleep(0.3)
        return said

    def click_first(colour, region):
        """Clicks the topmost line of this colour in a region: the first
        of several links that share it"""
        _, _, at = q.screen_raw()
        hits = [(x, y) for y in range(region[1], region[3])
                for x in range(region[0], region[2]) if at(x, y) == colour]
        if not hits:
            return None
        top = hits[0][1]
        line = [(x, y) for x, y in hits if y < top + 14]
        x0, x1 = min(x for x, _ in line), max(x for x, _ in line)
        y1 = max(y for _, y in line)
        q.click((x0 + x1) // 2, y1 - 3)
        q.mouse_goto(*away)
        return (x0, top, x1, y1)

    visit(e, "/page.html", "Web: Test page")
    visit(e, "/two.html", "Web: Page two")

    # --- the menu, and the history -------------------------------------
    seen = len(q.serial())
    menu(1)
    got = wait_title(q, geom, "Web: History")
    check(f"the menu's History opens the history in a tab ({got!r})",
          got == "Web: History" and "[web] menu: History" in q.serial()[seen:])
    time.sleep(1)
    said = [find("test page"), find("page two")]
    check(f"it lists the pages seen ({said})",
          all(s.endswith("1 of 1") for s in said))
    # "Clear the history" is the first of the red links, above the list
    clear = click_first((0xB0, 0x30, 0x30), page2)
    time.sleep(2)
    said = find("nothing visited")
    check(f"'Clear the history' empties it ({clear}, {said!r})",
          clear is not None and said.endswith("1 of 1"))

    # --- the cookies ---------------------------------------------------
    visit(e, "/lives", "Web: Lives")
    menu(3)
    got = wait_title(q, geom, "Web: Cookies")
    time.sleep(1)
    said = find("lasting")
    check(f"the cookies page lists what a site set ({got!r}, {said!r})",
          got == "Web: Cookies" and said.endswith("1 of 1"))
    clear = click_first((0xB0, 0x30, 0x30), page2)
    time.sleep(2)
    said = find("no cookies")
    visit(e, "/jar", "Web: Jar 1")
    check(f"'Remove all cookies' removes them: the site gets none "
          f"({said!r}, {JAR_SEEN[-1:]})", said.endswith("1 of 1") and
          JAR_SEEN[-1:] == [None])

    # --- the settings: JavaScript off, and on again ---------------------
    menu(4)
    got = wait_title(q, geom, "Web: Settings")
    time.sleep(1)
    tick = (0x20, 0x20, 0x24)
    _, n_before = pixels(q, *page2, tick)
    # The first box (JavaScript) is the one higher up
    _, _, at = q.screen_raw()
    first = min((y for y in range(page2[1], page2[3])
                 for x in range(page2[0], page2[2]) if at(x, y) == tick),
                default=None)
    if first is not None:
        xs = [x for x in range(page2[0], page2[2]) if at(x, first) == tick or
              at(x, first + 3) == tick]
        q.click(min(xs) + 3, first + 4)
        q.mouse_goto(*away)
        time.sleep(1)
    _, n_after = pixels(q, *page2, tick)
    seen = len(q.serial())
    q.sendkey("pgdn")
    time.sleep(1)
    save = click_first((0xE6, 0xE6, 0xEA), page2)
    end = time.time() + 15
    while time.time() < end and "settings saved" not in q.serial()[seen:]:
        time.sleep(0.3)
    check(f"in the settings, a click unticks JavaScript and the button saves "
          f"it ({n_before} -> {n_after} tick pixels)",
          n_after < n_before and save is not None and
          "[web] settings saved: javascript=0 images=1" in q.serial()[seen:])
    seen = len(q.serial())
    got = visit(e, "/js.html", "Web: JS page")
    time.sleep(2)
    check(f"and the next page's scripts do not run ({got!r})",
          q.window_title(*geom) == "Web: JS page" and
          "[js]" not in q.serial()[seen:])
    q.click(cx + define(SRC, "ADDR_X") + 60, cy + bar // 2)
    q.mouse_goto(*away)
    time.sleep(1)
    typed(q, "about:settings?save=1&js=on&images=on")
    q.sendkey("ret")
    wait_title(q, geom, "Web: Settings")
    got = visit(e, "/js.html", "Web: Scripted 5")
    check(f"ticked again, they run ({got!r})", got == "Web: Scripted 5")

    # --- a page may not lead to the browser's pages ----------------------
    visit(e, "/page.html", "Web: Test page")
    visit(e, "/evil.html", "Web: Evil")
    seen = len(q.serial())
    click_first((0x80, 0x60, 0x40), page2)
    time.sleep(2)
    blocked = "[web] blocked about:history?clear=all" in q.serial()[seen:]
    title = q.window_title(*geom)
    menu(1)
    wait_title(q, geom, "Web: History")
    time.sleep(1)
    said = find("test page")
    check(f"a page's link to about:history?clear=all is refused, and the "
          f"history stays ({title!r}, {said!r})", blocked and
          title == "Web: Evil" and said.endswith("1 of 1"))


def held(q, keys, down):
    """Keys held down, or let go - while the mouse clicks"""
    q.cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "key", "data": {"down": down,
                                 "key": {"type": "qcode", "data": k}}}
        for k in keys]}})
    time.sleep(0.3)


def ctrl_click(e):
    q, sw, sh, w0, geom = e.q, e.sw, e.sh, e.w0, e.geom
    cx, cy, bar, page = e.cx, e.cy, e.bar, e.page
    away = (sw - 20, sh // 2)
    tab_h, tab_max = define(SRC, "TAB_H"), define(SRC, "TAB_MAX")
    strip = cy + bar + tab_h // 2

    def tab_x(i, n):
        tw = min(tab_max, (w0 - 16) // n)
        return cx + 8 + i * tw + tw // 2, cx + 8 + i * tw + tw - 15

    def tabs_said(seen, want, timeout=15):
        end = time.time() + timeout
        said = []
        while time.time() < end:
            said = re.findall(r"\[web\] tabs: (\d+), at (\d+)", q.serial()[seen:])
            if said and said[-1] == want:
                break
            time.sleep(0.3)
        return said[-1] if said else None

    def link_click(colour, region, keys=(), button=None):
        box, _ = pixels(q, *region, colour)
        if not box:
            return None
        q.mouse_goto((box[0] + box[2]) // 2, box[3] - 2)
        time.sleep(0.3)
        held(q, keys, True)
        if button:
            q.mouse_button(True, button)
            q.mouse_button(False, button)
        else:
            q.mouse_button(True)
            q.mouse_button(False)
        held(q, keys, False)
        q.mouse_goto(*away)
        return box

    visit(e, "/page.html", "Web: Test page")
    # (the menu's pages are in tabs of their own by now: counted from
    # where things stand)
    now = re.findall(r"\[web\] tabs: (\d+), at (\d+)", q.serial())
    n, at = (int(now[-1][0]), int(now[-1][1])) if now else (1, 1)
    page2 = (page[0], page[1] + (tab_h if n > 1 else 0), page[2] - 20, page[3])
    seen = len(q.serial())
    link = link_click(LINK, page2, ["ctrl"])
    said = tabs_said(seen, (str(n + 1), str(at)))
    time.sleep(1)
    check(f"Ctrl and a click on a link open it in a tab behind this one "
          f"({link}, {said}, {q.window_title(*geom)!r})",
          said == (str(n + 1), str(at)) and
          q.window_title(*geom) == "Web: Test page"
          and "[web] ctrl-click: " in q.serial()[seen:])
    q.click(tab_x(at, n + 1)[0], strip)            # the one after this
    q.mouse_goto(*away)
    got = wait_title(q, geom, "Web: Page two")
    check(f"the new tab holds the page the link leads to ({got!r})",
          got == "Web: Page two")
    seen = len(q.serial())
    q.click(tab_x(at, n + 1)[1], strip)            # its cross
    q.mouse_goto(*away)
    tabs_said(seen, (str(n), str(at)))
    wait_title(q, geom, "Web: Test page")

    seen = len(q.serial())
    page2 = (page[0], page[1] + tab_h, page[2] - 20, page[3])
    link_click(LINK, page2, ["ctrl", "shift"])
    said = tabs_said(seen, (str(n + 1), str(at + 1)))
    got = wait_title(q, geom, "Web: Page two")
    check(f"with Shift as well, the tab opens in front ({said}, {got!r})",
          said == (str(n + 1), str(at + 1)) and got == "Web: Page two")

    visit(e, "/evil.html", "Web: Evil")
    region = (page[0], page[1], page[2], page[3])
    seen = len(q.serial())
    link_click((0x80, 0x60, 0x40), region, ["ctrl"])
    time.sleep(1.5)
    link_click((0x80, 0x60, 0x40), region, button="middle")
    time.sleep(1.5)
    said = q.serial()[seen:]
    check(f"Ctrl and the middle button alike refuse a page's link to "
          f"about:history ({said.count('[web] blocked about:history')} "
          f"refused, {q.window_title(*geom)!r})",
          said.count("[web] blocked about:history?clear=all") == 2 and
          "[web] tabs:" not in said and q.window_title(*geom) == "Web: Evil")


def main():
    return run("the browser's menu and its own pages", [ui, ctrl_click])


sys.exit(main())
