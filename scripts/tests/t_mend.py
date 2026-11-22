#!/usr/bin/env python3
"""ThreadOS test: Mend, the backup, on a stick.

user64/gmend.c (docs/weave/canvas/NotesMend.dc.html) keeps copies of
the home on a FAT stick (SYS64_VOLUME, kernel/vol.c): a list for every
day there is a copy of, the bytes of what changed that day beside it.
The machine boots from an ext2 disk with a home; a USB stick carries a
copy made two days before, put there with mtools - a note since
changed, a list the same, a file since deleted. Under Weave:

  Mend finds the stick by its label and the day on it, and says each
  file of Documents is changed since, the same, or deleted since -
  what comparing the copy's list with the home gives here; the screen
  says the files are copied, and names them with their states
  the changed note, chosen and brought back, comes back beside the one
  there is as "notes (Tuesday).txt", which is not touched
  Copy now: the day's list on the stick, read with mtools, names every
  file of the home but the dot ones, with their sizes, as the disk image
  has them; the bytes of what is new or changed are there, as the image
  holds them; what did not change is not copied again but pointed at
  the earlier day; and the thread has the new day as Today

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
from ptest import (check, summary, LOOK, _kernel_define, Ext2Image,
                   make_mbr_fat_disk, fat_put, fat_get, MTOOLS_ENV)

L = LOOK
N = theme("night")
SRC = "user64/gmend.c"
T = 1900000000                     # the files' time on the disk
OLD = "2030-06-11"                 # the copy on the stick: a Tuesday
FILES = {"Documents/notes.txt": b"notes, as they are now\n",
         "Documents/market-list.txt": b"linen\ndyes\n",
         "Documents/letters/to-ola.txt": b"Dear Ola\n"}
OLD_NOTES = b"old notes"


def d(name):
    return _kernel_define(SRC, name)


def make_stick(path):
    ok, start = make_mbr_fat_disk(path, 64, fat32=False, label="MENDSTICK")
    man = ("#mend 1 %d\n" % (T + 1000) +
           "Documents/notes.txt\t%d\t%d\t%s\n" % (len(OLD_NOTES), T - 5000, OLD) +
           "Documents/market-list.txt\t%d\t%d\t%s\n"
           % (len(FILES["Documents/market-list.txt"]), T, OLD) +
           "Documents/budget.csv\t6\t%d\t%s\n" % (T - 5000, OLD))
    subprocess.run(["mmd", "-i", "%s@@%d" % (path, start * 512), "::MEND",
                    "::MEND/days", "::MEND/store", "::MEND/store/" + OLD,
                    "::MEND/store/%s/Documents" % OLD], env=MTOOLS_ENV,
                   check=True, capture_output=True)
    good = all([
        fat_put(path, start, man.encode(), "/MEND/days/%s.txt" % OLD),
        fat_put(path, start, OLD_NOTES,
                "/MEND/store/%s/Documents/notes.txt" % OLD),
        fat_put(path, start, FILES["Documents/market-list.txt"],
                "/MEND/store/%s/Documents/market-list.txt" % OLD),
        fat_put(path, start, b"1,2,3\n",
                "/MEND/store/%s/Documents/budget.csv" % OLD)])
    return ok and good, start


def main():
    print("--- Mend ---")
    work = tempfile.mkdtemp()
    try:
        tree = os.path.join(work, "tree")
        for rel, data in FILES.items():
            p = os.path.join(tree, "home/admin", rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, "wb").write(data)
            os.utime(p, (T, T))
        os.makedirs(os.path.join(tree, "tmp"))
        img = os.path.join(work, "disk.img")
        subprocess.run(["truncate", "-s", "64M", img], check=True)
        subprocess.run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", "4096",
                        "-L", "ThreadOS", "-d", tree, img], check=True)
        stick = os.path.join(work, "stick.img")
        ok, start = make_stick(stick)
        check("mtools makes the stick and an earlier copy on it", ok)
        q = Qemu(KERNEL, INITRD, img, sata=True, usbdisk=stick, ram_mib=1024,
                 serial=os.path.join(work, "s.log"))
        try:
            today = run(q)
        finally:
            q.close()
        if today:
            after(img, stick, start, today)
    finally:
        subprocess.run(["rm", "-rf", work])
    return summary()


def walk_home(e):
    """The home's files but the dot ones, as the disk image has them"""
    out = {}
    for path in e.tree("/home/admin"):
        rel = path[len("/home/admin/"):]
        if any(part.startswith(".") for part in rel.split("/")):
            continue
        st = e.stat(path)
        if st and st["type"] == "regular":
            out[rel] = e.read(path)
    return out


def after(img, stick, start, today):
    e = Ext2Image(img)
    back = e.read("/home/admin/Documents/notes (Tuesday).txt")
    now = e.read("/home/admin/Documents/notes.txt")
    check(f"on the disk: the note brought back beside the one there is "
          f"({back!r}), which is untouched ({now!r})",
          back == OLD_NOTES and now == FILES["Documents/notes.txt"])
    home = walk_home(e)
    man = fat_get(stick, start, "/MEND/days/%s.txt" % today)
    lines = (man or b"").decode(errors="replace").splitlines()
    listed = {}
    for ln in lines[1:]:
        f = ln.split("\t")
        if len(f) == 4:
            listed[f[0]] = (int(f[1]), f[3])
    check(f"the day's list on the stick, read with mtools, names every file "
          f"of the home with its size ({sorted(listed)} - {sorted(home)})",
          lines and lines[0].startswith("#mend 1 ") and
          {k: v[0] for k, v in listed.items()} ==
          {k: len(v) for k, v in home.items()})
    copied = {k for k, v in listed.items() if v[1] == today}
    wrong = [k for k in copied
             if fat_get(stick, start, "/MEND/store/%s/%s" % (today, k))
             != home[k]]
    check(f"what is new or changed is on the stick, the bytes the disk has "
          f"({sorted(copied)}, wrong: {wrong})",
          copied and not wrong and "Documents/notes.txt" in copied)
    same = listed.get("Documents/market-list.txt", (0, ""))[1]
    check(f"what did not change is not copied again: it points at the day "
          f"its bytes are from ({same})",
          same == OLD and fat_get(stick, start, "/MEND/store/%s/Documents/"
                                  "market-list.txt" % today) is None)


def run(q):
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

    q.type_line("runw gmend")
    out = wait_after(0, "[mend] drawn", 60)
    m = re.search(r"\[mend\] drawn (\d+) (\d+)", out)
    cw = int(m.group(1)) if m else 0
    fw = front()
    check(f"Mend's window ({cw} wide, at {fw})", fw and cw > 0)
    if not fw:
        return None
    ox, oy = fw[0] + 1, fw[1] + L["WEAVE_TITLE_H"] + 1
    rows = dict(re.findall(r"\[mend\] row (\S+) ([a-z ]+?)\r?\n", out))
    check(f"Mend finds the stick and the day on it, and each file's state "
          f"against the home ({rows})",
          "[mend] stick MENDSTICK FAT16" in out and
          "[mend] days 1: %s" % OLD in out and
          rows == {"notes.txt": "changed since",
                   "market-list.txt": "the same",
                   "budget.csv": "deleted since"})
    scr = q.screen_raw()
    title = q.read_ui(ox + d("PAD_X") + d("BADGE") + 14, oy + d("PAD_Y") + 22,
                      300, px=d("STATE_PX"), face="interb", screen=scr)
    ry = d("CARD_Y") + d("CARD_HEAD")
    shown = []
    for k in range(3):
        name = q.read_ui(ox + d("PAD_X") + 40, oy + ry + k * d("ROW_H") + 24,
                         200, px=d("ROW_PX"), face="inter", screen=scr)
        shown.append(name)
    check(f"the screen: the files are copied ({title!r}), and Documents as "
          f"it was ({shown})", title == "Your files are copied" and
          shown == ["notes.txt", "market-list.txt", "budget.csv"])

    # the changed note, brought back beside the one there is
    seen = len(q.serial())
    q.click(ox + d("PAD_X") + 120, oy + ry + d("ROW_H") // 2)
    time.sleep(0.5)
    by = d("CARD_Y") + d("CARD_HEAD") + d("ROWS_SHOWN") * d("ROW_H") + 16
    q.click(ox + d("PAD_X") + 40, oy + by + d("BTN_H") // 2)
    q.mouse_goto(*away)
    out = wait_after(seen, "[mend] brought back", 15)
    check("the note brought back as \"notes (Tuesday).txt\"",
          "[mend] brought back Documents/notes.txt as notes (Tuesday).txt"
          in out)

    # Copy now
    bold = UiFont.shared(d("BTN_PX"), "interb")
    w0 = ((sum(bold.glyph(c)[0] for c in "Bring back notes.txt") + 63) >> 6) + 36
    seen = len(q.serial())
    q.click(ox + d("PAD_X") + w0 + 10 + 30, oy + by + d("BTN_H") // 2)
    q.mouse_goto(*away)
    out = wait_after(seen, "[mend] days 2", 60)
    m = re.search(r"\[mend\] days 2: %s (\S+)" % OLD, out)
    today = m.group(1) if m else None
    check(f"Copy now: copied to the stick, and the thread has a second day "
          f"({today})", "[mend] copied" in out and "to MENDSTICK" in out and
          today is not None)
    time.sleep(1)
    scr = q.screen_raw()
    # the second of two knots is at the thread's right end
    right = cw - d("PAD_X") - 10
    lab = q.find_ui(ox + right - 60, ox + right + 20,
                    oy + d("THREAD_Y") + 28, px=d("DAY_PX"), face="interb",
                    screen=scr)
    check(f"its knot is called Today, and chosen ({lab!r})", lab == "Today")
    q.sendkey("alt-f4")
    time.sleep(2)
    return today


if __name__ == "__main__":
    sys.exit(main())
