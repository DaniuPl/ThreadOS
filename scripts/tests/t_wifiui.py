#!/usr/bin/env python3
"""ThreadOS smoke test: the Wi-Fi networks in Settings.

Settings' Wi-Fi page shows what the driver (kernel/iwlwifi.c) has: the
card, its state, a button that starts a scan, and the networks the last
scan heard - the loudest first, with the name (or "Hidden network"),
the protection and the signal in four bars. QEMU has no Intel Wireless
3160, so the networks come from 'wifi test': frames of the shape 802.11
gives them, pushed through the same two handlers a scan uses, one of
them with a broken checksum. The page says the list is that, and what
it shows is read off the screen:

  ThreadOS-Test, WPA2, -52 dBm: four bars
  Guest, open, -74 dBm (a probe response): two bars
  a hidden WPA2 network, -81 dBm: one bar
  and no "Broken": its checksum was wrong

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, _kernel_define

_GS = "user/gsettings.c"
LIST_Y = _kernel_define(_GS, "WIFI_LIST_Y")
ROW_H = _kernel_define(_GS, "WIFI_ROW_H")
SEC_WIFI = GSET_SECTIONS.index("Wi-Fi")
DARK = theme("dark")


def open_wifi_page(q):
    W, H = q.size()
    q.launch_dock("gsettings")
    end = time.time() + 15
    while time.time() < end:
        for slot in range(8):
            x, y = cascade(slot, GSET_W + 2, GSET_H + TITLE_H + 2, w=W, h=H)
            if q.window_title(x, y, GSET_W + 2) == "Settings":
                cx, cy = x + 1, y + CONTENT_DY
                q.click(cx + GSET_SIDE_W // 2,
                        cy + GSET_SIDE_Y0 + SEC_WIFI * GSET_SIDE_H +
                        GSET_SIDE_H // 2)
                q.mouse_goto(W - 20, H // 2)
                time.sleep(1.5)
                return cx, cy
        time.sleep(0.3)
    return None


def row(q, cx, cy, i, shot):
    """Row i of the list: (its name, dimmed when hidden; its protection;
    how many bars are lit)"""
    top = cy + LIST_Y + i * ROW_H
    base = ui_font_baseline_in(UD_PX, top, ROW_H)
    name = q.read_ui(cx + GSET_PANE_X, base, 190, px=UD_PX,
                     fg=DARK["app_text"], screen=shot)
    if not name:
        name = "(dim) " + q.read_ui(cx + GSET_PANE_X, base, 190, px=UD_PX,
                                    fg=DARK["app_dim"], screen=shot)
    sec = q.read_ui(cx + GSET_PANE_X + 200, base, 48, px=UD_PX,
                    fg=DARK["app_dim"], screen=shot)
    _, _, at = shot
    bars = sum(1 for k in range(4)
               if at(cx + GSET_PANE_X + 250 + 5 * k + 1, top + 2 + 14) ==
               DARK["app_text"])
    return name, sec, bars


def main():
    slog = tempfile.mktemp(suffix=".log")
    q = Qemu(KERNEL, INITRD, serial=slog)
    try:
        check("the desktop came up", q.wait_desktop())
        q.mouse_home()
        at = open_wifi_page(q)
        check("Settings opens on Wi-Fi", at is not None)
        if at is None:
            return summary()
        cx, cy = at
        shot = q.screen_raw()
        before = row(q, cx, cy, 0, shot)
        check("before 'wifi test' there is no list on a machine with no card "
              "(%s)" % (before,), before[0] in ("", "(dim) "))

        q.click_desktop()
        out = q.command("wifi test", "networks read", timeout=20)
        check("'wifi test' reads three networks out of four frames",
              "networks read: 3" in out)
        W, H = q.size()
        q.mouse_goto(W - 20, H // 2)
        card = ""
        end = time.time() + 10             # the page redraws once a second
        while time.time() < end and card != "none in this machine":
            time.sleep(0.5)
            card = q.gset_info(cx, cy, 0)
        shot = q.screen_raw()
        check("the page says there is no card ('%s')" % card,
              card == "none in this machine")
        rows = [row(q, cx, cy, i, shot) for i in range(4)]
        check("the loudest first: ThreadOS-Test, WPA2, four bars (%s)"
              % (rows[0],), rows[0] == ("ThreadOS-Test", "WPA2", 4))
        check("then Guest, open, two bars (%s)" % (rows[1],),
              rows[1] == ("Guest", "open", 2))
        check("then the hidden one, by what it is, one bar (%s)"
              % (rows[2],), rows[2] == ("(dim) Hidden network", "WPA2", 1))
        check("and nothing of the frame with the broken checksum (%s)"
              % (rows[3],), rows[3][0] in ("", "(dim) ") and
              "Broken" not in str(rows))
    finally:
        q.close()
        if os.path.exists(slog):
            os.unlink(slog)
    return summary()


sys.exit(main())
