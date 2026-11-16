#!/usr/bin/env python3
"""ThreadOS smoke test: a window filling the screen, and full screen.

The green light fills the room between the bar and the dock - which
both stay, as a person asked: a full window that hides the dock is not
what the light is for. With "Hide the dock until the pointer reaches
it" ('dockhide') the dock goes, the room reaches the bottom of the
screen, and the dock comes out at the edge.

Full Screen, from the window tile's menu in the dock, is the other
thing: the whole screen, the bar and the dock out of the way until the
pointer reaches their edge. The bar coming out would cover the window's
title bar and the lights on it - the only way back - so the title bar
slides out under it, and its green light gives the window back.

All of it read off the screen: the dock's tiles under a filled window,
the window's title where the bar was, the terminal's ground where the
dock was, the Files tile's very picture at the bottom edge, the bar's
name and the title under it at the top, and the window back.

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, _kernel_define

TERM_H = _kernel_define("user/gterm.c", "H0")


def block(at, x, y, n):
    return tuple(at(x + i, y + j) for j in range(4, n - 4)
                 for i in range(4, n - 4))


def bar_name(q):
    """The bar's bold name: the program in front"""
    return q.find_ui(BAR_NAME_X, BAR_NAME_X + 100,
                     ui_font_baseline_in(BAR_PX, 0, BAR_H),
                     px=BAR_PX, face="bold")


def main():
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")      # (for the setting)
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    q = Qemu(KERNEL, INITRD, disk, serial=slog)
    try:
        check("the desktop came up", q.wait_desktop())
        W, H = q.size()
        q.mouse_home()
        q.mouse_goto(W // 2, H // 2)
        time.sleep(1)
        g = dock_geom(w=W, h=H)
        n = g["tile"]
        fx, fy = g["tiles"][0]
        _, _, at = q.screen_raw()
        files_pic = block(at, fx, fy, n)

        q.launch_dock("gterm")
        wx = wy = None
        end = time.time() + 15
        while time.time() < end and wx is None:
            time.sleep(0.5)
            for slot in range(8):
                x, y = cascade(slot, TERM_W + 2, TERM_H + TITLE_H + 2,
                               w=W, h=H)
                if "Terminal" in q.window_title(x, y, TERM_W + 2):
                    wx, wy = x, y
                    break
        check("a terminal opens", wx is not None)
        if wx is None:
            return summary()

        # The green light: the room between the bar and the dock
        q.click(*light_xy(wx, wy, 2))
        q.mouse_goto(W // 2, H // 2)
        time.sleep(2)
        _, _, at = q.screen_raw()
        g1 = dock_geom(n_w=1, w=W, h=H)
        gx, gy = g1["tiles"][0]
        title_at = q.window_title(0, BAR_H, W)
        check("the green light fills the room under the bar ('%s')"
              % title_at, "Terminal" in title_at)
        check("and the dock stays, the Files tile where it was",
              block(at, gx, gy, n) == files_pic and
              bar_name(q) == "Terminal")
        q.click(*light_xy(0, BAR_H, 2))
        q.mouse_goto(W // 2, H // 2)
        time.sleep(2)
        check("the green light again: its own size back",
              "Terminal" in q.window_title(wx, wy, TERM_W + 2))

        # Full Screen, from the window tile's menu
        q.mouse_goto(*dock_tile(DOCK_N, n_w=1, w=W, h=H))
        time.sleep(0.4)
        q.mouse_right_click()
        time.sleep(0.8)
        tx, _ = dock_tile(DOCK_N, n_w=1, w=W, h=H)
        mx = min(max(tx - TMENU_W // 2, 0), W - TMENU_W)
        my = g1["y"] - TMENU_GAP - (2 * MENU_PAD + 4 * MENU_ROW)
        q.click(mx + TMENU_W - 10, my + MENU_PAD + 2 * MENU_ROW +
                MENU_ROW // 2)
        q.mouse_goto(W // 2, H // 2)
        title = ""
        end = time.time() + 10
        while time.time() < end and "Terminal" not in title:
            time.sleep(0.5)
            title = q.window_title(0, 0, W)
        check("Full Screen: the window's title bar where the bar was "
              "('%s')" % title, "Terminal" in title)
        _, _, at = q.screen_raw()
        check("and the window where the dock was",
              block(at, fx, fy, n) != files_pic and
              at(fx + n // 2, fy + n // 2) == at(W // 2, H // 2 + 60))

        q.mouse_goto(W // 2, H - 1)
        time.sleep(1.5)
        _, _, at = q.screen_raw()
        check("the pointer at the bottom edge brings the dock out",
              block(at, gx, gy, n) == files_pic)
        q.mouse_goto(W // 2, H // 2)
        time.sleep(1.5)
        _, _, at = q.screen_raw()
        check("and it goes again when the pointer leaves",
              block(at, gx, gy, n) != files_pic)

        q.mouse_goto(W // 2, 1)
        time.sleep(1.5)
        name = bar_name(q)
        strip = q.window_title(0, BAR_H, W)
        check("the pointer at the top brings the bar out ('%s'), the "
              "window's title bar slid out under it ('%s')" % (name, strip),
              name == "Terminal" and "Terminal" in strip)

        q.click(*light_xy(0, BAR_H, 2))
        q.mouse_goto(W // 2, H // 2)
        back = ""
        end = time.time() + 10
        while time.time() < end and "Terminal" not in back:
            time.sleep(0.5)
            back = q.window_title(wx, wy, TERM_W + 2)
        check("its green light gives the window its own size and place "
              "back ('%s')" % back, "Terminal" in back)
        time.sleep(1)
        _, _, at = q.screen_raw()
        check("and the dock is there again", block(at, gx, gy, n) ==
              files_pic and bar_name(q) == "Terminal")

        # The dock hidden until the pointer comes
        q.click_desktop()
        check("'dockhide 1'", "saved" in q.sudo("set dockhide 1", "saved"))
        q.mouse_goto(W // 2, H // 2)
        end = time.time() + 10
        hidden = False
        while time.time() < end and not hidden:
            time.sleep(0.5)
            _, _, at = q.screen_raw()
            hidden = block(at, gx, gy, n) != files_pic
        check("with dockhide the dock goes", hidden)
        q.click(*light_xy(wx, wy, 2))
        q.mouse_goto(W // 2, H // 2)
        time.sleep(2)
        _, _, at = q.screen_raw()
        check("and a filled window reaches the screen's bottom",
              at(W // 2, H - 3) != aurora_px(W // 2, H - 3, W, H) and
              block(at, gx, gy, n) != files_pic)
        q.mouse_goto(W // 2, H - 1)
        time.sleep(1.5)
        _, _, at = q.screen_raw()
        check("the pointer at the bottom brings the dock over it",
              block(at, gx, gy, n) == files_pic)
    finally:
        q.close()
        for f in (slog, disk):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


sys.exit(main())
