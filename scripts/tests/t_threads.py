#!/usr/bin/env python3
"""ThreadOS smoke test: the shell's notes and the shuttle (Weave).

docs/protonui-plan.md stage 6, the designs in
docs/weave/canvas/Notify.dc.html and Shuttle.dc.html. Under Weave the
island is the shuttle: a capsule standing above the loom on a short
thread in the colour of whose it is. A note rides it for a moment and
then waits in Threads - the panel the loom's clock opens, one thread a
program - and the clock's knot counts what waits. A note has a weight:
a Quiet one never rides, an Urgent one rides until it is answered, even
with Quiet on. Its actions come back to the program as WEV_NOTE.

shuttledemo (libc/tests/shuttledemo.c) sends them through ProtonUI v2's
pu_notify and pu_shuttle, a key each, and says on the serial port what
it sent and what came back. The test reads the screen in the letters
the desktop draws (Inter, through FreeType) at the places look.h gives:

  a Normal note rides the shuttle - who, whose thread, its line, its
  action - on a thread of its program's dye (kernel/weave.c's letter
  dyes), then leaves it after a few seconds; the knot at the clock says
  1
  the clock opens Threads: the program's thread with "1 note", the note
  on it, its action under it
  a Quiet note never rides; the knot says 2, the thread "2 notes", the
  newest first
  an Urgent note rides with a madder edge, and is still there after
  longer than a Normal one rides; a click on its first action comes
  back to the program as action 1 of that note, and takes it away
  the Normal note's action, clicked in Threads, comes back to the
  program as action 1 of the Normal note; Mark read empties the thread
  and the knot goes
  Quiet, turned on in Threads: a Normal note no longer rides, and waits
  an Urgent one still rides
  a timer on the shuttle counts down by itself; a download shows its
  per cent and a bar as long; a call counts up in the colour it asked
  for; the end takes it away
  under the Mac style the island at the top carries a timer: its title
  and its count

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, LOOK, UiFont

L = LOOK
N = theme("night")
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(
    __file__))))
CS = UiFont.CHARSET + "·"
WHITE = (255, 255, 255)
NAME = "Shuttle demo"                    # shuttledemo's window


def rgb(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


def near(a, b, tol=24):
    return a is not None and all(abs(x - y) <= tol for x, y in zip(a, b))


def letter_dye(letter):
    """The dye of a program with no icon of its own, by kernel/weave.c's
    table and its first letter"""
    src = open(os.path.join(ROOT, "kernel", "weave.c")).read()
    m = re.search(r"letter_dyes\[\] = \{([^}]*)\}", src)
    dyes = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{6})", m.group(1))]
    return rgb(dyes[ord(letter) % len(dyes)])


def text_w(s, px, face):
    """ui_font_width(): the advances summed, rounded up"""
    f = UiFont.shared(px, face)
    return (sum(f.glyph(c)[0] for c in s) + 63) >> 6


class Geom:
    def __init__(self, W, H):
        self.W, self.H = W, H
        self.loom = H - L["LOOM_GAP"] - L["LOOM_H"]
        self.foot = self.loom - L["SHT_LIFT"]   # the shuttle's bottom
        right = W - L["LOOM_SIDE"] - 1 - L["LOOM_PAD"]
        self.clock = right - L["LOOM_CLOCK_W"]
        self.badge = (self.clock - 3, self.loom + 17)
        self.px = W - L["LOOM_SIDE"] - L["THR_W"]
        self.py = L["THR_GAP"]

    def note_box(self):
        w, h = L["SHT_NOTE_W"], L["SHT_NOTE_H"]
        return (self.W - w) // 2, self.foot - h, w, h


def capsule(q, g, h, screen=None):
    """Whether a capsule h high stands on the shuttle's thread: the
    shuttle's cloth inside it, at its top and its foot, and none just
    above it -> the colour of its edge (a line, or an Urgent note's
    madder two pixels deep), or None"""
    _, _, at = screen or q.screen_raw()
    y, cx = g.foot - h, g.W // 2
    if near(at(cx, y + 5), N["sht_bg"], 6) and \
            near(at(cx, y + h - 6), N["sht_bg"], 6) and \
            not near(at(cx, y - 3), N["sht_bg"], 6):
        return at(cx, y + 1)
    return None


def pills(x, w, labels):
    """Where the shuttle puts a note's actions, from its right end:
    (x, w) of each"""
    out, rx = [], x + w - L["SHT_ACT_RIGHT"]
    for lab in reversed(labels):
        aw = text_w(lab, L["SHT_ACT_PX"], "interb") + 2 * L["SHT_ACT_PAD"]
        rx -= aw
        out.insert(0, (rx, aw))
        rx -= L["SHT_ACT_GAP"]
    return out


def shuttle_note(q, room=220):
    """The note riding the shuttle: (who and whose, its line), read up to
    'room' pixels - short of its actions"""
    scr = q.screen_raw()
    x, y, w, h = Geom(*q.size()).note_box()
    if capsule(q, Geom(*q.size()), h, scr) is None:
        return ("", "")
    top = q.read_ui(x + L["SHT_TEXT_X"], y + L["SHT_LINE1_Y"], 220, px=12,
                    face="inter", charset=CS, fg=N["dot"], screen=scr)
    line = q.read_ui(x + L["SHT_TEXT_X"], y + L["SHT_LINE2_Y"], room, px=14,
                     face="interb", fg=N["menu_text"], screen=scr)
    return (top, line)


def badge(q, g):
    """What the knot at the clock says: '' when there is none"""
    scr = q.screen_raw()
    _, _, at = scr
    bx, by = g.badge
    if not near(at(bx - 6, by), N["accent"], 10):
        return ""
    return q.find_ui(bx - 5, bx + 5, ui_font_baseline_in(10, by - 9, 18),
                     px=10, face="interb", fg=WHITE, screen=scr)


def until(fn, want, timeout):
    end = time.time() + timeout
    got = fn()
    while time.time() < end and got != want:
        time.sleep(0.3)
        got = fn()
    return got


def thread_card(q, g, quiet=False):
    """Threads' first card: (name, count, [(title, line) of its notes])"""
    scr = q.screen_raw()
    cx, cw = g.px + L["THR_PAD"], L["THR_W"] - 2 * L["THR_PAD"]
    cy = g.py + L["THR_HEAD_H"] + (L["THR_BANNER_H"] if quiet else 0)
    nb = ui_font_baseline_in(L["THR_NAME_PX"], cy, L["THR_ROW_H"])
    name = q.read_ui(cx + 12 + L["THR_KNOT"] + 10, nb, 150,
                     px=L["THR_NAME_PX"], face="interb", fg=N["menu_text"],
                     screen=scr)
    chx = cx + cw - 22
    count = q.find_ui(chx - 80, chx - 4, nb, px=11, face="interb",
                      fg=N["dot"], screen=scr)
    items = []
    for i in range(L["THR_ITEMS"]):
        iy = cy + L["THR_ROW_H"] + i * L["THR_ITEM_H"]
        t = q.read_ui(cx + L["THR_ITEM_X"], iy + 15, 180, px=12,
                      face="inter", fg=N["dot"], screen=scr)
        if not t:
            break
        items.append((t, q.read_ui(cx + L["THR_ITEM_X"], iy + 34, 300, px=13,
                                   face="interb", fg=N["menu_text"],
                                   screen=scr)))
    return name, count, items, cy


def threads_title(q, g):
    return q.read_ui(g.px + 16, g.py + L["SELVEDGE_H"] + 14 + 20, 120,
                     px=L["THR_TITLE_PX"], face="interb", fg=N["menu_text"])


def main():
    print("--- the shuttle and Threads ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            run(q)
        finally:
            q.close()
    finally:
        for p in (disk, slog):
            if os.path.exists(p):
                os.remove(p)
    return summary()


def sent(q, key, what):
    """A key to shuttledemo, and the number of the note it sent"""
    before = q.serial().count("[shuttledemo] sent %s " % what)
    q.sendkey(key)
    end = time.time() + 10
    while time.time() < end:
        found = re.findall(r"\[shuttledemo\] sent %s (-?\d+)" % what,
                           q.serial())
        if len(found) > before:
            return int(found[-1])
        time.sleep(0.2)
    return None


def answered(q, num, timeout=8):
    """What shuttledemo heard back for note num: its answer, or None"""
    end = time.time() + timeout
    while time.time() < end:
        m = re.findall(r"\[shuttledemo\] answered %d (\d+)" % num, q.serial())
        if m:
            return int(m[-1])
        time.sleep(0.2)
    return None


def run(q):
    check("the desktop came up", q.wait_desktop())
    W, H = q.size()
    g = Geom(W, H)
    q.click_desktop()
    check("'layout weave'", "saved" in q.sudo("set layout weave", "saved"))
    end = time.time() + 10
    while time.time() < end and "the Weave style" not in q.serial():
        time.sleep(0.3)
    check("the desktop takes it", "the Weave style" in q.serial())
    q.type_line("runw shuttledemo")
    q.wait_serial("[shuttledemo] drawn", timeout=60)

    # The keys to shuttledemo: a click in its window, below its buttons.
    # Found by its selvedge once the window has opened - on a loaded
    # machine its opening takes longer than a moment
    accent = N["accent"]

    def selvedge():
        _, _, at = q.screen_raw()
        for y in range(0, H - 200):
            x = 0
            while x < W - 100:
                if at(x, y) == accent and at(x + 99, y) == accent and \
                        at(x + 5, y + 2) == accent and \
                        at(x + 5, y + 3) != accent:
                    return (x, y)
                x += 50
        return None
    # (the same place twice: a window still opening is a picture growing)
    win, last = None, None
    end = time.time() + 15
    while time.time() < end and not win:
        time.sleep(0.5)
        at_now = selvedge()
        win = at_now if at_now and at_now == last else None
        last = at_now
    check("shuttledemo's window, with the selvedge (%s)" % (win,), win)
    if not win:
        return
    q.click(win[0] + 150, win[1] + L["WEAVE_TITLE_H"] + 130)
    away = (W // 2, 60)
    q.mouse_goto(*away)

    dye = letter_dye(NAME[0])
    x, y, w, h = g.note_box()

    # --- a Normal note rides, then waits ---------------------------------
    n1 = sent(q, "n", "normal")
    check("a Normal note sent through pu_notify (number %s)" % n1,
          n1 is not None and n1 > 0)
    want = ("Ania · " + NAME, "Is the new build on the stick yet?")
    got = until(lambda: shuttle_note(q, 300), want, 8)
    t0 = time.time()
    check("it rides the shuttle above the loom: who and whose thread, its "
          "line (%s)" % (got,), got == want)
    scr = q.screen_raw()
    _, _, at = scr
    thread = at(W // 2, g.foot + L["SHT_LIFT"] // 2)
    avatar = at(x + 12 + 6, y + h // 2)
    check("on a thread of its program's dye, down to the loom (%s, %s)" %
          (thread, dye), near(thread, dye, 8))
    check("its avatar in the dye too (%s)" % (avatar,), near(avatar, dye, 8))
    ay = y + (h - L["SHT_ACT_H"]) // 2
    (px_, pw_), = pills(x, w, ["Reply"])
    act = q.read_ui(px_ + L["SHT_ACT_PAD"],
                    ui_font_baseline_in(L["SHT_ACT_PX"], ay, L["SHT_ACT_H"]),
                    pw_ - L["SHT_ACT_PAD"], px=L["SHT_ACT_PX"], face="interb",
                    fg=WHITE, screen=scr)
    check("its action at the right end, on the accent ('%s')" % act,
          act == "Reply")
    check("nothing waits yet: no knot at the clock ('%s')" % badge(q, g),
          badge(q, g) == "")
    got = until(lambda: shuttle_note(q, 300), ("", ""), 10)
    rode = time.time() - t0
    check("it leaves the shuttle by itself after a few seconds (%.1f s)" %
          rode, got == ("", "") and 2.5 <= rode <= 7.5)
    b = until(lambda: badge(q, g), "1", 5)
    check("and waits: the knot at the clock says 1 ('%s')" % b, b == "1")

    # --- Threads ------------------------------------------------------------
    q.click(g.clock + 40, g.loom + L["LOOM_H"] // 2)
    q.mouse_goto(*away)
    t = until(lambda: threads_title(q, g), "Threads", 5)
    check("the clock opens Threads ('%s')" % t, t == "Threads")
    name, count, items, cy = thread_card(q, g)
    check("the program's thread, with one note (%s, %s)" % (name, count),
          name == NAME and count == "1 note")
    check("the note on it (%s)" % items,
          items == [("Ania", "Is the new build on the stick yet?")])
    cx = g.px + L["THR_PAD"]
    fy = cy + L["THR_ROW_H"] + L["THR_ITEM_H"]
    act = q.read_ui(cx + L["THR_ITEM_X"] + 12,
                    ui_font_baseline_in(12, fy, L["THR_BTN_H"]),
                    text_w("Reply", 12, "interb") + 6, px=12, face="interb",
                    fg=WHITE)
    check("its action under it ('%s')" % act, act == "Reply")
    q.sendkey("esc")
    t = until(lambda: threads_title(q, g), "", 5)
    check("Esc puts Threads away ('%s')" % t, t == "")

    # --- a Quiet note never rides -------------------------------------------
    n2 = sent(q, "q", "quiet")
    check("a Quiet note sent (number %s)" % n2, n2 is not None and n2 > n1)
    end = time.time() + 3
    rides = 0
    while time.time() < end:
        rides += capsule(q, g, h) is not None
        time.sleep(0.2)
    check("it never rides the shuttle (%d frames seen with it)" % rides,
          rides == 0)
    b = until(lambda: badge(q, g), "2", 5)
    check("it waits at once: the knot says 2 ('%s')" % b, b == "2")
    q.click(g.clock + 40, g.loom + L["LOOM_H"] // 2)
    q.mouse_goto(*away)
    until(lambda: threads_title(q, g), "Threads", 5)
    name, count, items, cy = thread_card(q, g)
    check("on the same thread, the newest first (%s, %s, %s)" %
          (name, count, items),
          name == NAME and count == "2 notes" and
          items == [("Updates", "Darkroom 3.1 is ready"),
                    ("Ania", "Is the new build on the stick yet?")])
    q.sendkey("esc")
    until(lambda: threads_title(q, g), "", 5)

    # --- an Urgent note rides until it is answered ---------------------------
    n3 = sent(q, "u", "urgent")
    check("an Urgent note sent (number %s)" % n3, n3 is not None and n3 > n2)
    want = ("Alarm · " + NAME, "The tea is ready")
    got = until(lambda: shuttle_note(q), want, 8)
    check("it rides the shuttle (%s)" % (got,), got == want)
    edge = capsule(q, g, h)
    check("with a madder edge (%s)" % (edge,),
          near(edge, N["app_danger"], 40))
    end = time.time() + 8
    seen = []
    while time.time() < end:
        seen.append(shuttle_note(q) == want)
        time.sleep(0.3)
    check("still riding after eight seconds, longer than a Normal one "
          "rides (%d of %d looks)" % (sum(seen), len(seen)),
          seen and all(seen))
    _, _, at = q.screen_raw()
    stop, snooze = pills(x, w, ["Stop", "Snooze"])
    mid = at(stop[0] + 3, ay + L["SHT_ACT_H"] // 2)
    check("its first action, on the accent (%s at %s)" % (mid, stop),
          near(mid, N["accent"], 6))
    q.click(stop[0] + stop[1] // 2, ay + L["SHT_ACT_H"] // 2)
    q.mouse_goto(*away)
    a = answered(q, n3)
    check("a click on it comes back to the program: note %s, action %s" %
          (n3, a), a == 1)
    got = until(lambda: shuttle_note(q), ("", ""), 5)
    check("and the note is gone from the shuttle (%s)" % (got,),
          got == ("", ""))
    check("answered, it does not wait: the knot still says 2 ('%s')" %
          badge(q, g), badge(q, g) == "2")

    # --- an action in Threads, and Mark read --------------------------------
    q.click(g.clock + 40, g.loom + L["LOOM_H"] // 2)
    q.mouse_goto(*away)
    until(lambda: threads_title(q, g), "Threads", 5)
    name, count, items, cy = thread_card(q, g)
    fy = cy + L["THR_ROW_H"] + len(items) * L["THR_ITEM_H"]
    q.click(cx + L["THR_ITEM_X"] + 20, fy + L["THR_BTN_H"] // 2)
    q.mouse_goto(*away)
    a = answered(q, n1)
    check("the Normal note's action, clicked in Threads, comes back: note "
          "%s, action %s" % (n1, a), a == 1)
    name, count, items, cy = thread_card(q, g)
    check("and leaves the Quiet one on the thread (%s, %s)" % (count, items),
          count == "1 note" and items == [("Updates", "Darkroom 3.1 is ready")])
    fy = cy + L["THR_ROW_H"] + L["THR_ITEM_H"]
    cw = L["THR_W"] - 2 * L["THR_PAD"]
    q.click(cx + cw - 12 - 20, fy + L["THR_BTN_H"] // 2)
    q.mouse_goto(*away)
    end = time.time() + 5
    name = thread_card(q, g)[0]
    while time.time() < end and name:
        time.sleep(0.3)
        name = thread_card(q, g)[0]
    check("Mark read: the thread is gone ('%s')" % name, name == "")
    none = q.find_ui(g.px + 20, g.px + L["THR_W"] - 20,
                     g.py + L["THR_HEAD_H"] + 30, px=13, face="inter",
                     fg=N["dot"])
    check("Threads says nothing waits ('%s')" % none, none == "Nothing waits.")
    b = until(lambda: badge(q, g), "", 5)
    check("and the knot at the clock is gone ('%s')" % b, b == "")

    # --- Quiet ---------------------------------------------------------------
    qw = 24 + text_w("Quiet", L["THR_PILL_PX"], "interb") + 10
    qx = g.px + L["THR_W"] - 16 - qw
    hy = g.py + L["SELVEDGE_H"] + 14
    q.click(qx + qw // 2, hy + L["THR_PILL_H"] // 2)
    q.mouse_goto(*away)
    banner = until(lambda: q.read_ui(
        g.px + 28, ui_font_baseline_in(12, g.py + L["THR_HEAD_H"],
                                       L["THR_BANNER_H"] - 8), 320, px=12,
        face="inter", fg=N["menu_text"]),
        "Quiet: only Urgent rides the shuttle.", 5)
    check("Quiet turned on in Threads ('%s')" % banner,
          banner == "Quiet: only Urgent rides the shuttle.")
    q.sendkey("esc")
    until(lambda: threads_title(q, g), "", 5)
    n4 = sent(q, "n", "normal")
    end = time.time() + 3
    rides = 0
    while time.time() < end:
        rides += capsule(q, g, h) is not None
        time.sleep(0.2)
    check("with Quiet on a Normal note does not ride (%d looks with it)" %
          rides, n4 and rides == 0)
    b = until(lambda: badge(q, g), "1", 5)
    check("it waits: the knot says 1 ('%s')" % b, b == "1")
    n5 = sent(q, "u", "urgent")
    want = ("Alarm · " + NAME, "The tea is ready")
    got = until(lambda: shuttle_note(q), want, 8)
    check("an Urgent one still rides, even in Quiet (%s)" % (got,),
          n5 and got == want)
    q.click(snooze[0] + snooze[1] // 2, ay + L["SHT_ACT_H"] // 2)
    q.mouse_goto(*away)
    a = answered(q, n5)
    check("its second action comes back as action 2 (%s)" % a, a == 2)
    until(lambda: shuttle_note(q), ("", ""), 5)

    # --- what is going on: a timer, a download, a call ------------------------
    q.sendkey("t")
    q.wait_serial("[shuttledemo] shuttle timer 0", timeout=10)
    tw = text_w("Tea", 13, "interb")
    sw = 8 + L["SHT_RING"] + 12 + tw + 12 + text_w("00:00", 15, "interb") + 18
    sx, sh = (W - sw) // 2, L["SHT_TIMER_H"]
    sy = g.foot - sh
    base = ui_font_baseline_in(15, sy, sh)

    def timer():
        if capsule(q, g, sh) is None:
            return ("", "")
        scr = q.screen_raw()
        title = q.find_ui(sx + 8 + L["SHT_RING"] + 4,
                          sx + 8 + L["SHT_RING"] + 12 + tw + 4, base, px=13,
                          face="interb", fg=N["menu_text"], screen=scr)
        count = q.find_ui(sx + 8 + L["SHT_RING"] + 12 + tw + 6, sx + sw - 4,
                          base, px=15, face="interb", fg=dye, screen=scr)
        return (title, count)

    def secs(c):
        m = re.match(r"(\d+):(\d\d)$", c)
        return int(m.group(1)) * 60 + int(m.group(2)) if m else None

    end = time.time() + 8
    first = timer()
    while time.time() < end and secs(first[1]) is None:
        time.sleep(0.3)
        first = timer()
    time.sleep(2.5)
    second = timer()
    a, b = secs(first[1]), secs(second[1])
    check("a timer on the shuttle: its title and what is left (%s)" %
          (first,), first[0] == "Tea" and a is not None and 186 <= a <= 192)
    check("counting down by itself (%s, then %s)" % (first[1], second[1]),
          a is not None and b is not None and 1 <= a - b <= 5)

    q.sendkey("d")
    q.wait_serial("[shuttledemo] shuttle download 0", timeout=10)
    dw, dh = L["SHT_DL_W"], L["SHT_DL_H"]
    dx, dy = (W - dw) // 2, g.foot - dh

    def download():
        if capsule(q, g, dh) is None:
            return ""
        return q.find_ui(dx + dw - 80, dx + dw - 10, dy + 20, px=12,
                         face="inter", fg=N["dot"])

    pc = until(download, "41 %", 8)
    check("a download: its per cent ('%s')" % pc, pc == "41 %")
    # (the capsule grows like a spring, a little past its size and
    # back, and its contents wait while it is past it)
    bx, bw = dx + L["SHT_DL_X"], dw - L["SHT_DL_X"] - 18
    fill = bw * 41 // 100

    def bar():
        _, _, at = q.screen_raw()
        return at(bx + fill - 6, dy + 31), at(bx + fill + 8, dy + 31)

    end = time.time() + 5
    inside, past = bar()
    while time.time() < end and not near(inside, dye, 8):
        time.sleep(0.3)
        inside, past = bar()
    check("and a bar as long, in the program's dye (%s, then %s)" %
          (inside, past), near(inside, dye, 8) and not near(past, dye, 40))

    q.sendkey("c")
    q.wait_serial("[shuttledemo] shuttle call 0", timeout=10)

    def call():
        if capsule(q, g, dh) is None:
            return ("", "")
        scr = q.screen_raw()
        return (q.read_ui(dx + L["SHT_DL_X"], dy + 21, 120, px=13,
                          face="interb", fg=N["menu_text"], screen=scr),
                q.find_ui(dx + dw - 70, dx + dw - 10,
                          ui_font_baseline_in(13, dy, dh), px=13,
                          face="interb", fg=rgb(0x6A4FD6), screen=scr))

    end = time.time() + 8
    c = call()
    while time.time() < end and not (c[0] == "Ania" and secs(c[1]) is not None
                                     and secs(c[1]) >= 2):
        time.sleep(0.3)
        c = call()
    check("a call: who, and how long it has gone on, counting up in the "
          "colour it asked for (%s)" % (c,),
          c[0] == "Ania" and secs(c[1]) is not None and 2 <= secs(c[1]) <= 10)

    q.sendkey("e")
    q.wait_serial("[shuttledemo] shuttle end 0", timeout=10)
    end = time.time() + 5
    while time.time() < end and capsule(q, g, dh) is not None:
        time.sleep(0.3)
    check("the end takes it off the shuttle",
          capsule(q, g, dh) is None and capsule(q, g, sh) is None)

    # --- the other styles: the black island at the top ----------------------
    # A timer set under Weave goes on when the style changes; the Mac
    # style's island shows its title and its count
    q.sendkey("t")
    q.wait_serial("[shuttledemo] shuttle timer 0", timeout=10)
    q.click_desktop()
    q.sudo("set layout mac", "saved")
    end = time.time() + 10
    while time.time() < end and "the Mac style" not in q.serial():
        time.sleep(0.3)
    q.mouse_goto(W // 2, H // 2)
    ix, iy = (W - L["ISL_PLAY_W"]) // 2, L["ISL_TOP"]
    ib = ui_font_baseline_in(L["ISL_PX"], iy, L["ISL_PLAY_H"])

    def island():
        _, _, at = scr = q.screen_raw()
        if at(W // 2, iy + 4) != (0, 0, 0):
            return ("", "")
        return (q.read_ui(ix + L["ISL_NAME_X"], ib, 60, px=L["ISL_PX"],
                          fg=WHITE, screen=scr),
                q.find_ui(ix + L["ISL_PLAY_W"] // 2,
                          ix + L["ISL_PLAY_W"] - 4, ib, px=L["ISL_PX"],
                          face="bold", screen=scr))

    end = time.time() + 8
    got = island()
    while time.time() < end and not (got[0] == "Tea" and secs(got[1])):
        time.sleep(0.3)
        got = island()
    check("under the Mac style the island at the top carries it: its "
          "title and what is left (%s)" % (got,),
          got[0] == "Tea" and secs(got[1]) is not None and
          150 <= secs(got[1]) <= 192)


if __name__ == "__main__":
    sys.exit(main())
