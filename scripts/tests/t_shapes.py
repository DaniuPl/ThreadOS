#!/usr/bin/env python3
"""ThreadOS smoke test: soft-edged shapes, held up against exact areas.

The desktop's round corners, circles, lights and lines come out of
kernel/gfx.c, which decides how much of each pixel a shape covers from
the distance between the pixel's centre and the shape's edge. That is
an approximation of the real thing - the AREA of the pixel the shape
covers - and the honest test of an approximation is the thing itself.

So the system draws shapes white on black and prints them as numbers
('shape rrect|ring|line ...'), and this test works out the true area of
the same shapes by cutting every pixel into 16 x 16 and counting the
pieces inside. It asserts:

  that a straight edge on a whole pixel is exact: inside is 255,
  outside is 0, nothing in between - no smearing of a rectangle

  that every pixel is within a few 1/255ths of the true area, and on
  average within a fraction of one - corners, circles, a ring one
  pixel thick, lines at odd angles, a dot

  that the blur is the one kernel/gfx.c says it is - two passes of a
  box, each way, rounded the way it says - to the last bit, including
  the pixels around it that it must read and never write

on both kernels, because the 32-bit one does its arithmetic in a
narrower register and the two must still draw the same pixels.
"""

import math
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64
from ptest import in_rrect, in_line, shape_area as area
from ptest import check_area as compare

def parse(text, pw, ph, channels):
    rows = []
    for line in text.replace("\r", "").split("\n"):
        line = line.strip()
        if len(line) == pw * 2 * channels and re.fullmatch(r"[0-9a-f]+", line):
            b = bytes.fromhex(line)
            if channels == 1:
                rows.append(list(b))
            else:
                rows.append([(b[i], b[i + 1], b[i + 2])
                             for i in range(0, len(b), 3)])
    return rows[-ph:] if len(rows) >= ph else None


# --- the blur, from its definition --------------------------------------

def box(vals, r):
    """One pass of the box over a list: len(vals) - 2r averages, each
    the sum times 65536/n rounded, plus a half, over 65536"""
    n = 2 * r + 1
    inv = (65536 + n // 2) // n
    out, s = [], sum(vals[:n])
    for i in range(len(vals) - 2 * r):
        out.append((s * inv + 32768) >> 16)
        if i + n < len(vals):
            s += vals[i + n] - vals[i]
    return out


def blur_ref(pw, ph, r):
    def pat(i, j):
        return ((i * 37 + j * 11) & 0xFF,
                230 if ((i // 4 + j // 3) & 1) else 20,
                (i * j * 7) & 0xFF)
    img = [[pat(i, j) for i in range(pw)] for j in range(ph)]
    x, y, w, h = 2, 2, pw - 4, ph - 4
    e = 2 * r
    out = [row[:] for row in img]
    for ch in range(3):
        ext = []
        for j in range(h + 2 * e):
            sy = min(max(y - e + j, 0), ph - 1)
            ext.append([img[sy][min(max(x - e + i, 0), pw - 1)][ch]
                        for i in range(w + 2 * e)])
        a = [box(row, r) for row in ext]
        cols = list(zip(*a))
        a = list(zip(*[box(list(c), r) for c in cols]))
        a = [box(list(row), r) for row in a]
        cols = list(zip(*a))
        a = list(zip(*[box(list(c), r) for c in cols]))
        for j in range(h):
            for i in range(w):
                px = list(out[y + j][x + i])
                px[ch] = a[j][i]
                out[y + j][x + i] = tuple(px)
    return out


def shapes(q, tag, full=True):
    plan = [
        ("a rounded rectangle on whole pixels", "rrect", (32, 32, 480, 320, 128),
         lambda x, y: in_rrect(x, y, 2, 2, 30, 20, 8), 36, 26),
        ("a rounded rectangle between pixels", "rrect", (53, 41, 483, 327, 116),
         lambda x, y: in_rrect(x, y, 53 / 16, 41 / 16, 483 / 16, 327 / 16, 116 / 16),
         40, 28),
        ("a circle", "rrect", (40, 24, 400, 400, 200),
         lambda x, y: in_rrect(x, y, 2.5, 1.5, 25, 25, 12.5), 30, 28),
        ("a ring a pixel thick", "ring", (24, 24, 512, 352, 160, 16),
         lambda x, y: in_rrect(x, y, 1.5, 1.5, 32, 22, 10) and
         not in_rrect(x, y, 2.5, 2.5, 30, 20, 9), 36, 26),
        ("a thick ring round a circle", "ring", (40, 40, 400, 400, 200, 56),
         lambda x, y: in_rrect(x, y, 2.5, 2.5, 25, 25, 12.5) and
         not in_rrect(x, y, 6, 6, 18, 18, 9), 30, 30),
        ("a line at an odd angle", "line", (53, 75, 563, 322, 24),
         lambda x, y: in_line(x, y, 53 / 16, 75 / 16, 563 / 16, 322 / 16, 1.5),
         40, 26),
        ("a steep thick line", "line", (100, 20, 160, 400, 48),
         lambda x, y: in_line(x, y, 100 / 16, 20 / 16, 10, 25, 3), 18, 30),
        ("a dot", "line", (200, 200, 200, 200, 120),
         lambda x, y: in_line(x, y, 12.5, 12.5, 12.5, 12.5, 7.5), 26, 26),
    ]
    if not full:
        plan = plan[1:2] + plan[5:6]
    for name, kind, args, inside, pw, ph in plan:
        out = q.command("shape %s %s %d %d" % (kind, " ".join(map(str, args)),
                                             pw, ph), "drawn", timeout=30)
        ours = parse(out, pw, ph, 1)
        if name.startswith("a rounded rectangle on whole"):
            # Away from the corners the edge is straight and on a pixel
            # boundary: the true area is 0 or 255 and nothing else
            straight = [ours[y][x] for y in range(ph) for x in range(pw)
                        if ours and (10 <= x <= 23 or 10 <= y <= 13)] if ours else []
            check("%s%s: straight edges are exact - only 0 and 255 there"
                  % (tag, name), bool(straight) and set(straight) <= {0, 255})
        compare(tag + name, ours, area(inside, pw, ph))

    for r, pw, ph in ((2, 30, 20), (3, 40, 30)) if full else ((3, 40, 30),):
        out = q.command("shape blur %d %d %d" % (r, pw, ph), "shape: blur",
                        timeout=30)
        ours = parse(out, pw, ph, 3)
        ref = blur_ref(pw, ph, r)
        bad = -1 if ours is None else sum(
            1 for a, b in zip(ours, ref) for p, s in zip(a, b) if p != s)
        check("%sthe blur, radius %d on %dx%d: every pixel as defined, the "
              "border left alone (%d differ)" % (tag, r, pw, ph, bad), bad == 0)


def main():
    print()
    print("--- soft-edged shapes against their exact areas ---")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
    try:
        q.wait_serial("thread>", timeout=60)
        shapes(q, "")
    finally:
        q.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    print("--- the same under the 64-bit kernel ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    check("the disc with the 64-bit kernel builds",
          make_iso64(d, iso, text=True))
    if not os.path.exists(iso):
        return summary()
    q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
             qemu="qemu-system-x86_64")
    try:
        q.wait_serial("thread64>", timeout=120)
        shapes(q, "(64) ", full=False)
    finally:
        q.close()
        for f in (iso, slog):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


sys.exit(main())
