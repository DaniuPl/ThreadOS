#!/usr/bin/env python3
"""ThreadOS smoke test: pairing a Bluetooth phone from Settings.

The made-up adapter of t_bt (scripts/btadapter.py) in the xHCI socket,
a phone and a watch around it, and the desktop. Settings' Bluetooth
page searches, and the phone appears by its name; a click picks it and
Pair starts Secure Simple Pairing. The six digits the controller made
up come up on the page, large, with "Does Pixel 8 show this number?"
and Yes and No; Yes pairs, the page says so and the phone's row says
Paired - and the controller was told yes, once. Then Forget, and the
row no longer says Paired.

Run on its own or through scripts/run_tests.py.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, _kernel_define
from usbredir import Redir
from btadapter import BtAdapter, USER_CONFIRM

_GS = "user/gsettings.c"
BT_ROW_H = _kernel_define(_GS, "BT_ROW_H")
BT_PAIRED_X = _kernel_define(_GS, "BT_PAIRED_X")
BT_ASK_H = _kernel_define(_GS, "BT_ASK_H")
BT_ASK_YES_X = _kernel_define(_GS, "BT_ASK_YES_X")
SEC_BT = GSET_SECTIONS.index("Bluetooth")
DARK = theme("dark")
SEARCH_Y = GSET_PANE_Y0 + 4 * GSET_INFO_H + 8
LIST_Y = SEARCH_Y + GSET_CHIP_H + 10
ASK_Y = GSET_H - GSET_NOTE_DY - 14 - BT_ASK_H + 6


def open_bt_page(q):
    W, H = q.size()
    q.launch_dock("gsettings")
    end = time.time() + 15
    while time.time() < end:
        for slot in range(8):
            x, y = cascade(slot, GSET_W + 2, GSET_H + TITLE_H + 2, w=W, h=H)
            if q.window_title(x, y, GSET_W + 2) == "Settings":
                cx, cy = x + 1, y + CONTENT_DY
                q.click(cx + GSET_SIDE_W // 2,
                        cy + GSET_SIDE_Y0 + SEC_BT * GSET_SIDE_H +
                        GSET_SIDE_H // 2)
                q.mouse_goto(W - 20, H // 2)
                time.sleep(1.5)
                return cx, cy
        time.sleep(0.3)
    return None


def row(q, cx, cy, i, shot=None):
    """Row i of the device list: its name, and 'Paired' or ''"""
    top = cy + LIST_Y + i * BT_ROW_H
    base = ui_font_baseline_in(UD_PX, top, BT_ROW_H)
    name = q.read_ui(cx + GSET_PANE_X, base, 200, px=UD_PX,
                     fg=DARK["app_text"], screen=shot)
    paired = q.read_ui(cx + GSET_PANE_X + BT_PAIRED_X, base, 60, px=UD_PX,
                       fg=DARK["app_dim"], screen=shot)
    return name, paired


def page_note(q, cx, cy, shot=None):
    top = GSET_H - GSET_NOTE_DY - 14
    return q.read_ui(cx + GSET_PANE_X, cy + ui_font_baseline_in(11, top, 16),
                     GSET_W - GSET_PANE_X - 8, px=11, fg=DARK["app_dim"],
                     screen=shot)


def number(q, cx, cy, shot=None):
    """The six digits of a pairing, large and bold"""
    px = UD_PX + 6
    return q.read_ui(cx + GSET_PANE_X,
                     cy + ui_font_baseline_in(px, ASK_Y, GSET_CHIP_H), 100,
                     px=px, face="bold", fg=DARK["app_text"], screen=shot)


def wait_for(fn, want, timeout):
    end = time.time() + timeout
    got = fn()
    while time.time() < end and not want(got):
        time.sleep(0.4)
        got = fn()
    return got


def main():
    print()
    print("--- pairing a Bluetooth phone from Settings ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    bt = BtAdapter(eir=True)
    r = Redir(bt)
    work = tempfile.mkdtemp()
    disk = os.path.join(work, "disk.img")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        q = Qemu(KERNEL, INITRD, disk=disk, sata=True,
                 serial=os.path.join(work, "s.log"),
                 devices=["-device", "qemu-xhci,id=xhci", *r.qemu_args(0)])
        try:
            check("the desktop came up", q.wait_desktop())
            q.wait_serial("[ OK ] Bluetooth:", timeout=60)
            q.mouse_home()
            at = open_bt_page(q)
            check("Settings opens on Bluetooth", at is not None)
            if at is None:
                return summary()
            cx, cy = at
            W, H = q.size()
            q.click(cx + GSET_PANE_X + 75, cy + SEARCH_Y + GSET_CHIP_H // 2)
            q.mouse_goto(W - 20, H // 2)
            found = wait_for(lambda: [row(q, cx, cy, i) for i in range(3)],
                             lambda rows: any(n.startswith("Pixel 8")
                                              for n, _ in rows), 40)
            names = [n for n, _ in found]
            check("a search finds the phone by its name (%s)" % names,
                  any(n.startswith("Pixel 8") for n in names))
            i = next((k for k, n in enumerate(names)
                      if n.startswith("Pixel 8")), None)
            if i is None:
                return summary()
            # The list fills while the search runs; Pair waits for its end
            base = cy + ui_font_baseline_in(UD_PX, SEARCH_Y, GSET_CHIP_H)
            wait_for(lambda: q.read_ui(cx + GSET_PANE_X, base, 240,
                                       px=UD_PX, fg=DARK["app_dim"]),
                     lambda t: not t.startswith("Searching"), 30)
            q.click(cx + GSET_PANE_X + 40, cy + LIST_Y + i * BT_ROW_H +
                    BT_ROW_H // 2)
            time.sleep(1.2)
            q.click(cx + GSET_PANE_X + 40, cy + ASK_Y + GSET_CHIP_H // 2)
            q.mouse_goto(W - 20, H // 2)
            digits = wait_for(lambda: number(q, cx, cy),
                              lambda d: len(d) == 6, 40)
            said = page_note(q, cx, cy)
            want = "%06d" % bt.number if bt.number is not None else "?"
            ok = digits == want and said == "Does Pixel 8 show this number?"
            check("Pick and Pair: the page shows the six digits the "
                  "controller made up (%s; %s) and asks ('%s')"
                  % (digits, want, said), ok)
            if not ok:
                print("\n".join("   | " + l for l in q.serial()
                                 .splitlines()[-12:]))
            q.click(cx + GSET_PANE_X + BT_ASK_YES_X + 30,
                    cy + ASK_Y + GSET_CHIP_H // 2)
            q.mouse_goto(W - 20, H // 2)
            said = wait_for(lambda: page_note(q, cx, cy),
                            lambda s: s.startswith(("Paired", "Not paired")),
                            40)
            mark = wait_for(lambda: row(q, cx, cy, i)[1],
                            lambda s: s == "Paired", 10)
            check("Yes: paired - the page says so, the phone's row says "
                  "Paired, the controller was told yes once ('%s', '%s')"
                  % (said, mark),
                  said == "Paired with Pixel 8." and mark == "Paired" and
                  len(bt.sent(USER_CONFIRM)) == 1)
            # Forget it: still picked, its button now Forget
            q.click(cx + GSET_PANE_X + 40, cy + ASK_Y + GSET_CHIP_H // 2)
            q.mouse_goto(W - 20, H // 2)
            mark = wait_for(lambda: row(q, cx, cy, i)[1],
                            lambda s: s == "", 10)
            check("Forget: the row no longer says Paired ('%s')" % mark,
                  mark == "")
        finally:
            q.close()
    finally:
        r.close()
        for f in os.listdir(work):
            os.unlink(os.path.join(work, f))
        os.rmdir(work)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
