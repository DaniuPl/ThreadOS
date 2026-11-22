#!/usr/bin/env python3
"""ThreadOS test: Find, one field for everything on the machine.

user64/gfind.c (docs/weave/canvas/Find.dc.html) looks for words in
file names, inside files, in Settings, among the programs and in Weft's
history. The disk this test boots from holds a home with a project, a
journal, a file named for the word, one outside the home and Weft's
history; what Find should find is worked out here, independently, by
walking the same tree and reading every file the way grep -n would.
Under Weave:

  "selvedge" typed and Enter: the counts it says on the serial port are
  the ones worked out here, for every kind
  the field reads "selvedge", the count beside it says as many; each
  pill reads its name and count where the layout puts it; the first
  group's heading and its first row read off the screen
  the Inside files pill: up and down go through every line the word is
  on, file by file, as grep finds them; the line on the right is marked
  where the word is
  Enter opens Code at that file and line; Show in Files opens Files
  "Changed: today" finds no file (they were all made long before the
  machine's today), and back to any time finds them again; "In: home"
  leaves out the file outside the home
  "pointer speed": the Settings row, which opens Settings at Mouse;
  "calc": the Calculator, which starts
  every row of Find's index of Settings is a label in its section of
  user/gsettings.c, and every program it lists is in the initrd

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
from ptest import check, summary, LOOK, _kernel_define

L = LOOK
N = theme("night")
SRC = "user64/gfind.c"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(
    __file__))))
WORD = "selvedge"
KINDS = ["Files", "Inside files", "Settings", "Programs", "Weft history"]


def d(name):
    return _kernel_define(SRC, name)


def table(name):
    """A two-column string table of gfind.c's: [(a, b)]"""
    text = open(os.path.join(ROOT, SRC)).read()
    body = text[text.index(name):]
    body = body[:body.index("\n};")]
    return re.findall(r'\{ "([^"]*)", "([^"]*)" \}', body)


def make_tree(tree):
    k = os.path.join(tree, "home/admin/Projects/threados/kernel")
    os.makedirs(k)
    os.makedirs(os.path.join(tree, "home/admin/docs"))
    os.makedirs(os.path.join(tree, "tmp"))
    lines = ["/* line %d */" % i for i in range(1, 27)]
    lines += ["/* The selvedge: three pixels of the focus dye along",
              " * the top of the window in front */",
              "void weave_selvedge(int x, int y, int w, unsigned dye)",
              "{",
              "    gfx_fill(x, y, w, SELVEDGE_H, dye, 255);",
              "}"]
    open(os.path.join(k, "weave.c"), "w").write("\n".join(lines) + "\n")
    open(os.path.join(k, "look.h"), "w").write(
        "#define SELVEDGE_H 3    /* the thread along the window */\n")
    open(os.path.join(tree, "home/admin/docs/JOURNAL.md"), "w").write(
        "# Journal\n\nThe selvedge marks focus.\nNothing here.\n")
    open(os.path.join(tree, "home/admin/selvedge-notes.txt"), "w").write(
        "notes\n")
    # binary, with the word in it: not text, so not looked inside
    open(os.path.join(tree, "home/admin/blob.bin"), "wb").write(
        b"\0\1\2selvedge\0")
    # outside the home
    open(os.path.join(tree, "tmp/selvedge.log"), "w").write("a log\n")
    open(os.path.join(tree, "home/admin/.weft"), "w").write(
        "ThreadOS Weft 1\n"
        "history\thttps://en.wikipedia.org/wiki/Selvage\t"
        "Selvedge - what it is, in weaving\n"
        "history\thttps://example.org/\tExample\n")


def expected(tree, word, under="/"):
    """What Find should find, by walking the tree as it does: dot names
    left out, text files up to 256 KiB read for the word on each line"""
    w = word.lower()
    files, inside = [], {}
    top = os.path.join(tree, under.lstrip("/"))
    for dirpath, dirnames, filenames in os.walk(top):
        dirnames[:] = [x for x in dirnames if not x.startswith(".")]
        for f in filenames:
            if f.startswith("."):
                continue
            if w in f.lower():
                files.append(f)
            data = open(os.path.join(dirpath, f), "rb").read()
            if not data or len(data) > 256 * 1024 or b"\0" in data[:1024]:
                continue
            got = [i + 1 for i, ln in
                   enumerate(data.decode("latin-1").split("\n"))
                   if w in ln.lower()]
            if got:
                inside[f] = got
    settings = [a for a, b in table("settings[] = {")
                if w in a.lower() or w in b.lower()]
    programs = [a for a, b in table("programs[] = {")
                if w in a.lower() or w in b.lower()]
    web = []
    for ln in open(os.path.join(tree, "home/admin/.weft")):
        p = ln.rstrip("\n").split("\t")
        if p[0] == "history" and (w in p[1].lower() or w in p[2].lower()):
            web.append(p[2])
    return files, inside, settings, programs, web


def counts_of(out, word):
    m = re.search(r"\[find\] searched %s: (\d+) found - files (\d+), "
                  r"inside (\d+), settings (\d+), programs (\d+), web (\d+)"
                  % re.escape(word), out)
    return [int(x) for x in m.groups()] if m else None


def main():
    print("--- Find ---")
    work = tempfile.mkdtemp()
    try:
        tree = os.path.join(work, "tree")
        make_tree(tree)
        img = os.path.join(work, "disk.img")
        subprocess.run(["truncate", "-s", "64M", img], check=True)
        subprocess.run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", "4096",
                        "-L", "ThreadOS", "-d", tree, img], check=True)
        static_checks()
        q = Qemu(KERNEL, INITRD, img, sata=True, ram_mib=1024,
                 serial=os.path.join(work, "s.log"))
        try:
            run(q, tree)
        finally:
            q.close()
    finally:
        subprocess.run(["rm", "-rf", work])
    return summary()


def static_checks():
    gs = open(os.path.join(ROOT, "user/gsettings.c")).read()
    sections = re.search(r"static const char \*const sections\[\] = \{(.*?)\};",
                         gs, re.S).group(1)
    names = re.findall(r'"([^"]+)"', sections)
    fn = {"Wi-Fi": "draw_wifi", "Bluetooth": "draw_bluetooth",
          "Network": "draw_network", "Sound": "draw_sound",
          "General": "draw_general", "Appearance": "draw_look",
          "Desktop & Dock": "draw_desk", "Display": "draw_display",
          "Screen Lock": "draw_lockscreen", "Mouse": "draw_mouse",
          "Files": "draw_files", "Region": "draw_region",
          "Devices": "draw_devices", "Updates": "draw_updates"}
    wrong = []
    for label, section in table("settings[] = {"):
        if section not in names:
            wrong.append((label, section))
            continue
        if label == section:
            continue
        m = re.search(r"static void %s\(void\)\n\{(.*?)\n\}\n" % fn[section],
                      gs, re.S)
        if not m or '"%s"' % label not in m.group(1):
            wrong.append((label, section))
    check(f"every row of Find's index of Settings is a label in its section "
          f"of gsettings.c ({len(table('settings[] = {'))} rows, wrong: "
          f"{wrong})", not wrong)
    mk = open(os.path.join(ROOT, "Makefile")).read()
    missing = [p for _, p in table("programs[] = {")
               if not re.search(r"\b%s=\$\(BUILD\)" % re.escape(p), mk) and
               p != "web"]
    check(f"every program Find lists is in the initrd (missing: {missing})",
          not missing)


def run(q, tree):
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

    def front_title(timeout=30):
        w = front(timeout)
        if not w:
            return ""
        x0 = w[0] + L["WEAVE_BAR_LEFT"] + L["WEAVE_TITLE_ICON"] + 2
        base = ui_font_baseline_in(L["WEAVE_TITLE_PX"],
                                   w[1] + L["SELVEDGE_H"], L["WEAVE_BAR_H"])
        return q.find_ui(x0, x0 + 260, base, px=L["WEAVE_TITLE_PX"],
                         face="interb")

    def wait_title(want, timeout=60):
        end = time.time() + timeout
        t = ""
        while time.time() < end:
            t = front_title(5)
            if t.startswith(want):
                return t
            time.sleep(1)
        return t

    def back_to_find():
        for _ in range(8):
            if front_title(4).startswith("Find"):
                return True
            q.sendkey("alt-tab")
            time.sleep(0.5)
        return front_title(4).startswith("Find")

    def wait_after(seen, text, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            out = q.serial()[seen:]
            if text in out:
                return out
            time.sleep(0.3)
        return q.serial()[seen:]

    def search(word):
        seen = len(q.serial())
        q.sendkey("esc")
        q.type_line(word, enter=False)
        q.sendkey("ret")
        out = wait_after(seen, "[find] searched %s:" % word, 60)
        time.sleep(0.8)
        return counts_of(out, word)

    q.type_line("runw gfind")
    out = wait_after(0, "[find] drawn", 60)
    m = re.search(r"\[find\] drawn (\d+) (\d+)", out)
    cw, ch = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
    fw = front()
    check(f"Find's window ({cw}x{ch} at {fw})", fw and cw > 0)
    if not fw:
        return
    ox, oy = fw[0] + 1, fw[1] + L["WEAVE_TITLE_H"] + 1

    files, inside, settings, programs, web = expected(tree, WORD)
    want = [len(files), len(inside), len(settings), len(programs), len(web)]
    got = search(WORD)
    check(f"\"{WORD}\": what Find found is what walking the tree finds "
          f"(files {files}, inside {sorted(inside)}, settings {settings}, "
          f"programs {programs}, web {web}; Find said {got})",
          got == [sum(want)] + want and want[0] >= 2 and want[1] >= 3)

    # the screen: the field, the count, the pills where the layout puts them
    sans, bold = UiFont.shared(d("PILL_PX"), "inter"), \
        UiFont.shared(d("PILL_PX"), "interb")

    def width(font, s):
        return (sum(font.glyph(c)[0] for c in s) + 63) >> 6
    scr = q.screen_raw()
    _, _, at = scr
    field = q.read_ui(ox + d("SIDE") + d("FIELD_TEXT_X"),
                      ui_font_baseline_in(d("FIELD_PX"), oy + d("FIELD_Y"),
                                          d("FIELD_H")),
                      200, px=d("FIELD_PX"), face="inter", screen=scr)
    count = q.find_ui(ox + cw // 2, ox + cw - d("SIDE"),
                      ui_font_baseline_in(d("COUNT_PX"), oy + d("FIELD_Y"),
                                          d("FIELD_H")),
                      px=d("COUNT_PX"), face="inter", screen=scr)
    check(f"the field reads the word and the count beside it says as many "
          f"({field!r}, {count!r})",
          field == WORD and count.startswith("%d found in " % sum(want)))
    names = ["Everything"] + KINDS
    nums = [sum(want)] + want
    x, y, right = d("SIDE"), d("PILL_Y"), cw - d("SIDE")
    read = []
    for i, nm in enumerate(names):
        w = d("PILL_PAD") + d("PILL_DOT") + 8 + width(bold, nm) + 8 + \
            width(sans, str(nums[i])) + d("PILL_PAD")
        if x + w > right and x > d("SIDE"):
            x, y = d("SIDE"), y + d("PILL_H") + d("PILL_GAP")
        tx = x + d("PILL_PAD") + d("PILL_DOT") + 8
        pb = ui_font_baseline_in(d("PILL_PX"), oy + y, d("PILL_H"))
        a = q.read_ui(ox + tx, pb, width(bold, nm) + 4, px=d("PILL_PX"),
                      face="interb", screen=scr)
        b = q.read_ui(ox + tx + width(bold, nm) + 8, pb,
                      width(sans, str(nums[i])) + 4, px=d("PILL_PX"),
                      face="inter", screen=scr)
        read.append("%s %s" % (a, b))
        x += w + d("PILL_GAP")
    pill_rows_y = y
    check(f"each pill its name and its count ({read})",
          read == ["%s %d" % (n, c) for n, c in zip(names, nums)])
    # when and where, at the right of the last row or on one of their own
    wc = width(sans, "Changed: any time") + 2 * d("PILL_PAD")
    wi = width(sans, "In: everywhere") + 2 * d("PILL_PAD")
    if x + wc + d("PILL_GAP") + wi > right:
        pill_rows_y += d("PILL_H") + d("PILL_GAP")
    changed_xy = (ox + right - wi - d("PILL_GAP") - wc // 2,
                  oy + pill_rows_y + d("PILL_H") // 2)
    in_xy = (ox + right - wi // 2, oy + pill_rows_y + d("PILL_H") // 2)
    top_h = pill_rows_y + d("PILL_H") + d("TOP_PAD")
    lw = cw * d("LIST_PCT") // 100
    head = q.read_ui(ox + d("LIST_PAD") + 24, oy + top_h + d("LIST_TOP") + 10,
                     160, px=d("HEAD_PX"), face="interb", screen=scr)
    row_y = top_h + d("LIST_TOP") + d("HEAD_H")
    first = q.read_ui(ox + d("LIST_PAD") + 10 + d("ROW_TILE") + 12,
                      oy + row_y + 21, lw - 120, px=d("ROW_NAME_PX"),
                      face="interb", screen=scr)
    check(f"the first group's heading and its first row ({head!r}, "
          f"{first!r})", head == "FILES" and first in files)

    # Inside files: up and down through every line, file by file
    seen = len(q.serial())
    x, y = d("SIDE"), d("PILL_Y")
    for i, nm in enumerate(names[:3]):
        w = d("PILL_PAD") + d("PILL_DOT") + 8 + width(bold, nm) + 8 + \
            width(sans, str(nums[i])) + d("PILL_PAD")
        if x + w > right and x > d("SIDE"):
            x, y = d("SIDE"), y + d("PILL_H") + d("PILL_GAP")
        if i == 2:
            q.click(ox + x + w // 2, oy + y + d("PILL_H") // 2)
        x += w + d("PILL_GAP")
    q.mouse_goto(*away)
    out = wait_after(seen, "[find] at", 10)
    check("the Inside files pill", "[find] filter Inside files" in out)
    total = sum(len(v) for v in inside.values())
    for _ in range(total):            # one more than there are: it stays
        q.sendkey("down")
        time.sleep(0.25)
    time.sleep(0.8)
    stops = re.findall(r"\[find\] at ([^:\s]+):(\d+)", q.serial()[seen:])
    walked = {}
    order = []
    for name, line in stops:
        if not order or order[-1] != name:
            order.append(name)
        walked.setdefault(name, []).append(int(line))
    check(f"up and down go through every line the word is on, as grep "
          f"finds them ({walked} - {inside})",
          walked == inside and len(order) == len(inside) and
          len(stops) == total)
    last_name, last_line = stops[-1] if stops else ("", "0")

    # the buttons: right of the name, or under it where there is no room
    right_p = cw - d("PREV_PAD")
    w0 = width(UiFont.shared(d("BTN_PX"), "interb"), "Open in Code") + 28
    w1 = width(UiFont.shared(d("BTN_PX"), "inter"), "Show in Files") + 28
    bx0 = right_p - w1 - 8 - w0
    room = bx0 - 12 - (lw + 1 + d("PREV_PAD") + d("PREV_TILE") + 12)
    by = top_h + d("PREV_TOP") + (d("PREV_TILE") - d("BTN_H")) // 2
    code_y = top_h + d("PREV_TOP") + d("PREV_TILE") + d("CODE_GAP")
    if room < 160:
        by = top_h + d("PREV_TOP") + d("PREV_TILE") + 10
        code_y = by + d("BTN_H") + d("CODE_GAP")

    # the lines on the right: the word underlined in the accent, under
    # the line the match is on as under every other line it is on
    _, _, at = q.screen_raw()
    marks = 0
    px0 = ox + lw + 1 + d("PREV_PAD")
    for yy in range(oy + code_y, oy + ch - 30):
        run = sum(1 for xx in range(px0 + d("NUM_W"), ox + cw - d("PREV_PAD"))
                  if near(at(xx, yy), acc, 6))
        if run >= 20:
            marks += 1
    most = 2 * len(inside.get(last_name, []))
    check(f"its lines on the right, the word underlined in the accent: on "
          f"the line the match is on, and on no more lines than "
          f"{last_name} has it on ({marks} rows, at most {most})",
          2 <= marks <= most and marks % 2 == 0)

    # Enter opens Code at that file and line
    seen = len(q.serial())
    q.sendkey("ret")
    out = wait_after(seen, "[code] opened", 60)
    check(f"Enter opens Code at {last_name}:{last_line}",
          re.search(r"\[find\] open code \S+/%s %s" % (re.escape(last_name),
                                                      last_line), out) and
          "[code] opened %s at line %s" % (last_name, last_line) in out)
    wait_title("Code", 30)
    q.sendkey("alt-f4")
    time.sleep(2)
    check("Code closed, Find in front again", back_to_find())

    # Show in Files
    seen = len(q.serial())
    q.click(ox + right_p - w1 // 2, oy + by + d("BTN_H") // 2)
    out = wait_after(seen, "[find] open files", 10)
    title = wait_title("Files", 30)
    check(f"Show in Files opens Files ({title!r})",
          "[find] open files /" in out and title.startswith("Files"))
    q.sendkey("alt-f4")
    time.sleep(2)
    back_to_find()

    # Changed: today finds no file; any time again finds them
    def click_wait(xy, text):
        seen = len(q.serial())
        q.click(*xy)
        q.mouse_goto(*away)
        return wait_after(seen, text, 60)
    # (the field still holds the word: Enter now would open the file)
    out = click_wait(changed_xy, "[find] searched %s:" % WORD)
    today = counts_of(out, WORD)
    check(f"Changed: today - no file was changed on this machine's today "
          f"({today})", "[find] changed today" in out and today and
          today[1] == 0 and today[2] == 0 and today[3:] == want[2:])
    for _ in range(3):
        out = click_wait(changed_xy, "[find] searched %s:" % WORD)
    check(f"and round to any time, they are found again "
          f"({counts_of(out, WORD)})", "[find] changed any time" in out and
          counts_of(out, WORD) == [sum(want)] + want)
    out = click_wait(in_xy, "[find] searched %s:" % WORD)
    hf, hi, _, _, _ = expected(tree, WORD, "/home/admin")
    homec = counts_of(out, WORD)
    check(f"In: home leaves out the file outside the home ({homec}; "
          f"files {hf})", "[find] in home" in out and homec and
          homec[1] == len(hf) and homec[1] < want[0])
    click_wait(in_xy, "[find] searched %s:" % WORD)

    # a setting, and a program
    got = search("pointer speed")
    check(f"\"pointer speed\": the Settings row ({got})",
          got and got[3] == 1 and got[0] == 1)
    seen = len(q.serial())
    q.sendkey("ret")
    out = wait_after(seen, "[settings] opened at", 30)
    said = [ln.strip() for ln in out.splitlines()
            if "[find]" in ln or "[settings]" in ln or "[kernel]" in ln]
    check(f"which opens Settings at Mouse ({said})",
          "[find] open settings Mouse" in out and
          "[settings] opened at Mouse" in out)
    wait_title("Settings", 30)
    q.sendkey("alt-f4")
    time.sleep(2)
    back_to_find()
    got = search("calc")
    check(f"\"calc\": the Calculator ({got})", got and got[4] == 1)
    seen = len(q.serial())
    q.sendkey("ret")
    out = wait_after(seen, "[find] open program", 10)
    title = wait_title("Calc", 30)
    check(f"which starts ({title!r})",
          "[find] open program gcalc" in out and title.startswith("Calc"))


if __name__ == "__main__":
    sys.exit(main())
