#!/usr/bin/env python3
"""ThreadOS test: Notes, notes and lists in the serif.

user64/gnotes.c (docs/weave/canvas/NotesMend.dc.html) keeps a note as a
plain text file in ~/Notes: its title, "[ ] " and "[x] " items, what is
written. The disk this test boots from holds three, each changed at a
time of its own. Under Weave:

  the list is the three, newest first, as their files' times put them,
  their titles read off the screen in Inter's bold
  the newest open: its day, worked out here from its file's time; its
  title in the serif's bold and its items in the serif, read with
  FreeType from fonts/serif.ttf and serifb.ttf; the ticked item struck
  through and its knot filled
  a click on an item's knot ticks it, and the file says so at once
  typing at the end of what is written, kept a moment after
  Ctrl+N: a note typed with its title and an item made with Ctrl+L,
  kept in a file named after it
  Ctrl+D on another note asks, and its madder yes deletes the file
  and afterwards, from the disk image itself: each file holds exactly
  what was done to it

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, LOOK, _kernel_define, Ext2Image

L = LOOK
N = theme("night")
SRC = "user64/gnotes.c"
UG = "user/ugui.h"
MARKET = ("For the market on Saturday\n"
          "[x] the linen, folded\n"
          "[ ] the dyes for Ola\n"
          "[ ] a bag for the plums\n"
          "[ ] cash for the dyers\n"
          "\n"
          "Ask at the dyers' stall whether the indigo comes in a smaller "
          "jar.\n")
NOTES = {"For the market on Saturday": (MARKET, 0),
         "Books Ola asked for": ("Books Ola asked for\nThe one about dyes\n",
                                 -200),
         "Recipes": ("Recipes\nPlum cake, the old way\n", -300)}


def d(name):
    return _kernel_define(SRC, name)


def main():
    print("--- Notes ---")
    work = tempfile.mkdtemp()
    try:
        tree = os.path.join(work, "tree")
        nd = os.path.join(tree, "home/admin/Notes")
        os.makedirs(nd)
        os.makedirs(os.path.join(tree, "tmp"))
        t0 = int(time.time()) - 86400 * 3
        times = {}
        for title, (text, dt) in NOTES.items():
            p = os.path.join(nd, title + ".txt")
            open(p, "w").write(text)
            os.utime(p, (t0 + dt, t0 + dt))
            times[title] = t0 + dt
        img = os.path.join(work, "disk.img")
        subprocess.run(["truncate", "-s", "64M", img], check=True)
        subprocess.run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", "4096",
                        "-L", "ThreadOS", "-d", tree, img], check=True)
        q = Qemu(KERNEL, INITRD, img, sata=True, ram_mib=1024,
                 serial=os.path.join(work, "s.log"))
        try:
            run(q, times)
        finally:
            q.close()
        after(img)
    finally:
        subprocess.run(["rm", "-rf", work])
    return summary()


def after(img):
    e = Ext2Image(img)
    base = "/home/admin/Notes/"
    market = e.read(base + "For the market on Saturday.txt")
    want = MARKET.replace("[ ] the dyes for Ola", "[x] the dyes for Ola") \
        .replace("smaller jar.\n", "smaller jar. Bring a basket.\n")
    check(f"on the disk: the market note ticked and written to, nothing "
          f"else changed ({market!r})",
          market is not None and market.decode() == want)
    groc = e.read(base + "Groceries.txt")
    check(f"the new note, named after its title, its item made with Ctrl+L "
          f"({groc!r})", groc == b"Groceries\n[ ] bread\n")
    check("and the deleted one is gone", not e.exists(base + "Recipes.txt"))


def run(q, times):
    check("the desktop came up", q.wait_desktop())
    W, H = q.size()
    away = (W - 4, H // 2)
    q.click_desktop()
    check("'layout weave'", "saved" in q.sudo("set layout weave", "saved"))
    end = time.time() + 10
    while time.time() < end and "the Weave style" not in q.serial():
        time.sleep(0.3)
    acc = N["accent"]
    R = L["WEAVE_WIN_R"]

    def front(timeout=30):
        last = None
        end = time.time() + timeout
        while time.time() < end:
            q.mouse_goto(*away)
            _, _, at = q.screen_raw()
            got = None
            for y in range(0, H - 200):
                for x in range(0, W - 100, 2):
                    if at(x, y) == acc and at(x + 99, y) == acc and \
                            at(x, y + 2) == acc and at(x, y + 3) != acc:
                        got = (x - R, y)
                        break
                if got:
                    break
            if got and got == last:
                return got
            last = got
            time.sleep(0.5)
        return None

    def wait_after(seen, text, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            out = q.serial()[seen:]
            if text in out:
                return out
            time.sleep(0.3)
        return q.serial()[seen:]

    q.type_line("runw gnotes")
    out = wait_after(0, "[notes] drawn", 60)
    m = re.search(r"\[notes\] drawn (\d+) (\d+)", out)
    cw, ch = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
    fw = front()
    check(f"Notes' window ({cw}x{ch} at {fw})", fw and cw > 0)
    if not fw:
        return
    ox, oy = fw[0] + 1, fw[1] + L["WEAVE_TITLE_H"] + 1
    order = sorted(times, key=lambda t: -times[t])
    m = re.search(r"\[notes\] list (\d+): ([^\r\n]*)", out)
    listed = m.group(2).split(" | ") if m else []
    scr = q.screen_raw()
    _, _, at = scr
    nav_y = d("NAV_PAD") + d("NEW_H") + d("NOTE_GAP") + 4
    shown = [q.read_ui(ox + d("NAV_PAD") + 24,
                       oy + nav_y + i * (d("NOTE_H") + d("NOTE_GAP")) + 22,
                       d("NAV_W") - 40, px=d("NOTE_PX"), face="interb",
                       screen=scr) for i in range(3)]
    check(f"the list is the three, newest first ({listed}; on the screen "
          f"{shown})", listed == order and shown == order)

    # the open note: its day, its title, its items
    ax = d("NAV_W") + 1 + d("ART_X")
    t = time.gmtime(times[order[0]])
    day = "%s, %d %s · in Notes" % (time.strftime("%A", t), t.tm_mday,
                                          time.strftime("%B", t))
    date = q.read_ui(ox + ax, oy + d("ART_Y") + d("DATE_PX"), 300,
                     px=d("DATE_PX"), face="inter", screen=scr,
                     charset=UiFont.CHARSET + "·")
    y0 = d("ART_Y") + d("DATE_PX") + d("BLOCK_GAP")
    tbase = y0 + (d("TITLE_LINE") + d("TITLE_PX") * 73 // 100) // 2
    title = q.read_ui(ox + ax, oy + tbase, cw - ax - 40, px=d("TITLE_PX"),
                      face="serifb", screen=scr)
    check(f"its day, from its file's time ({date!r} - {day!r}), and its "
          f"title in the serif's bold ({title!r})",
          date == day and title == order[0])
    iy0 = y0 + d("TITLE_LINE") + d("BLOCK_GAP")
    step = d("ITEM_H") + d("ITEM_GAP")
    ibase = [iy0 + k * step + (d("ITEM_H") + d("ITEM_PX") * 73 // 100) // 2
             for k in range(4)]
    tx = ax + d("KNOT") + d("KNOT_GAP")
    items = [q.read_ui(ox + tx, oy + b, cw - tx - 40, px=d("ITEM_PX"),
                       face="serif", screen=scr) for b in ibase[1:]]
    check(f"its items in the serif ({items})",
          items == ["the dyes for Ola", "a bag for the plums",
                    "cash for the dyers"])

    def knot(k):
        ky = ibase[k] - d("ITEM_PX") * 73 // 200 - d("KNOT") // 2
        return (ox + ax + d("KNOT") // 2, oy + ky + d("KNOT") // 2)
    kx, ky = knot(0)
    done, open_ = at(kx, ky), at(*knot(1))
    sfont = UiFont.shared(d("ITEM_PX"), "serif")
    sw = (sum(sfont.glyph(c)[0] for c in "the linen, folded") + 63) >> 6
    sy = oy + ibase[0] - d("ITEM_PX") * 30 // 100
    ink = (0x6A, 0x70, 0x80)          # gnotes.c's ink for what is done, Night
    strike = sum(1 for x in range(ox + tx + 2, ox + tx + sw - 2)
                 if near(at(x, sy), ink, 8))
    check(f"the ticked item: its knot filled ({done}), an open one hollow "
          f"({open_}), and struck through ({strike} of {sw - 4} pixels)",
          near(done, acc, 8) and not near(open_, acc, 40) and
          strike >= sw - 8)

    # a click on a knot ticks it, and the file is kept at once
    seen = len(q.serial())
    q.click(*knot(1))
    q.mouse_goto(*away)
    out = wait_after(seen, "[notes] saved", 10)
    time.sleep(0.6)
    check("a click on \"the dyes for Ola\"'s knot ticks it and keeps it",
          "[notes] ticked 2 on" in out and
          "For the market on Saturday.txt" in out and
          near(q.screen_raw()[2](*knot(1)), acc, 8))

    # typing at the end of what is written
    for _ in range(6):
        q.sendkey("down")
        time.sleep(0.1)
    q.sendkey("end")
    seen = len(q.serial())
    q.type_line(" Bring a basket.", enter=False)
    out = wait_after(seen, "[notes] saved", 15)
    check("typing at the end of what is written is kept a moment after",
          "[notes] saved" in out)

    # a new note
    seen = len(q.serial())
    q.sendkey("ctrl-n")
    time.sleep(0.5)
    q.type_line("Groceries", enter=True)
    q.sendkey("ctrl-l")
    q.type_line("bread", enter=False)
    out = wait_after(seen, "Groceries.txt", 15)
    check("Ctrl+N: a new note, kept as Groceries.txt",
          "[notes] new" in out and "Groceries.txt" in out)

    # Ctrl+D on Recipes, now the fourth: asked, and deleted
    rec = nav_y + 3 * (d("NOTE_H") + d("NOTE_GAP")) + d("NOTE_H") // 2
    seen = len(q.serial())
    q.click(ox + d("NAV_W") // 2, oy + rec)
    wait_after(seen, "[notes] opened Recipes", 10)
    q.sendkey("ctrl-d")
    time.sleep(1)
    gw = min(cw - 40, _kernel_define(UG, "PU_SHEET_W"))
    gh = _kernel_define(UG, "PU_SHEET_H")
    gx, gy = (cw - gw) // 2, (ch - gh) // 2
    bh, bw = 32, (gw - 40 - 10) // 2
    by = gy + gh - bh - 16
    yes_x = gx + 20 + bw + 10
    scr = q.screen_raw()
    sheet = q.read_ui(ox + gx + 20, oy + gy + 36, gw - 40, px=15,
                      face="interb", screen=scr)
    yes = scr[2](ox + yes_x + 6, oy + by + bh // 2)
    q.click(ox + yes_x + bw // 2, oy + by + bh // 2)
    q.mouse_goto(*away)
    out = wait_after(seen, "[notes] deleted", 10)
    check(f"Ctrl+D asks ({sheet!r}), its yes madder ({yes}), and deletes "
          f"the note", sheet == "Delete this note?" and
          near(yes, N["app_danger"], 10) and
          "[notes] deleted Recipes" in out)
    # closed, so that what is typed is all on the disk
    seen = len(q.serial())
    q.sendkey("alt-f4")
    time.sleep(3)


if __name__ == "__main__":
    sys.exit(main())
