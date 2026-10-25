#!/usr/bin/env python3
"""ThreadOS smoke test: the network and Bluetooth on the bar.

Two icons left of 'show the desktop', lit when there is something behind
them. A click hangs a card from the bar that says what there is, and why
not when there is not. Held to what QEMU gives the machine:

  an RTL8139 card and, once DHCP answers, the address 10.0.2.15 - the
  network icon lit, the card naming both

  no USB controller, so no Bluetooth adapter - the Bluetooth icon dim,
  and its card saying so

  a click anywhere else puts the card away

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

DARK = theme("dark")
NET_X = bar_show_x() - BAR_ITEM_GAP - STAT_W
BT_X = NET_X - BAR_ITEM_GAP - STAT_W


def card_lines(q, icon_x, n):
    """The card's lines under its bold title, as the desktop lays them.
    The card is frosted glass, a different shade a few pixels along, so
    what lies under a letter is taken from the same column just above
    the line's letters."""
    x = icon_x + STAT_W // 2 - STAT_CARD_W // 2
    x = max(6, min(x, SCREEN_W - 6 - STAT_CARD_W))
    top = BAR_H + 6 + STAT_PAD
    shot = q.screen_raw()
    at = shot[2]
    out = []
    for i in range(n + 1):
        row = top + i * STAT_ROW
        out.append(q.read_ui(x + STAT_PAD,
                             ui_font_baseline_in(UI_PX, row, STAT_ROW),
                             STAT_CARD_W - 2 * STAT_PAD, px=UI_PX,
                             face="bold" if i == 0 else "sans",
                             fg=DARK["menu_text"], screen=shot,
                             under=lambda xx, yy, r=row: at(xx, r + 1)))
    return out


# A place on the bar with nothing under it: the pointer rests here, so
# that the bar (which hides when it is not used) stays, and no hint
# covers anything
REST = (220, BAR_H // 2)


def open_card(q, icon_x):
    q.mouse_goto(*REST)
    time.sleep(1)
    q.click(icon_x + STAT_W // 2, BAR_H // 2)
    q.mouse_goto(*REST)
    time.sleep(1)


def lit(at, x):
    """The brightest pixel of an icon on the bar"""
    y0 = (BAR_H - 16) // 2
    return max(sum(at(x + dx, y0 + dy)) for dx in range(STAT_W)
               for dy in range(16))


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=tempfile.mktemp(suffix=".log"))
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()

            # --- the network ----------------------------------------------
            got = []
            for _ in range(30):            # until DHCP has answered
                open_card(q, NET_X)
                got = card_lines(q, NET_X, 3)
                if got[2] == "Address: 10.0.2.15":
                    break
                open_card(q, NET_X)        # put away, to open again
            
            check("a click on the network icon opens its card (%s)" % got,
                  got[0] == "Network" and got[1] == "Wired: RTL8139")
            check("with the address QEMU gave", got[2] == "Address: 10.0.2.15")
            check("and an honest word on Wi-Fi ('%s')" % got[3],
                  got[3] == "Wi-Fi: no adapter this system can drive")

            q.click(300, 300)              # anywhere else
            q.mouse_goto(*REST)
            time.sleep(1)
            w, h, at = q.screen_raw()
            check("a click elsewhere puts the card away",
                  card_lines(q, NET_X, 1)[0] == "")

            # --- Bluetooth --------------------------------------------------
            check("the network icon is lit and Bluetooth's dim (%d > %d)"
                  % (lit(at, NET_X), lit(at, BT_X)),
                  lit(at, NET_X) > lit(at, BT_X) + 150)
            open_card(q, BT_X)
            got = card_lines(q, BT_X, 1)
            check("Bluetooth's card says there is no adapter (%s)" % got,
                  got[0] == "Bluetooth" and got[1].startswith("No adapter"))
        finally:
            q.close()
    finally:
        os.unlink(disk)
    return summary()


sys.exit(main())
