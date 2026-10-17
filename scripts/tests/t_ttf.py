#!/usr/bin/env python3
"""ThreadOS smoke test: TrueType glyphs, held up against FreeType.

Until now the system drew every letter from one 8x8 bitmap. kernel/
ttf.c reads TrueType fonts and draws their glyphs with anti-aliasing,
and the only honest way to say it draws them RIGHT is to compare it
with a renderer everybody already trusts. So the system prints glyphs
as numbers ('font sheet'), and this test draws the same glyphs from
the same file with FreeType and compares:

  the character map: for every character in the font, the glyph the
  system picks is the one fontTools reads out of the file

  the advance: how far the pen moves, to 1/64 of a pixel, equal to
  FreeType's

  the box: the size of the bitmap and where it sits against the pen
  and the baseline, equal to FreeType's - a pixel off here and text
  would wobble

  the pixels: every coverage value within a few 1/255ths of FreeType's.
  Both compute the exact area under the outline; what differs is only
  where the arcs get cut into lines, and rounding

both fonts, sizes from small to large, and both kernels - the 32-bit
one does its 64-bit divisions with a loop of its own, the 64-bit one
with the processor's, and both paths have to give the same glyphs.

FreeType is drawn with hinting off, because the system does not run
hinting programs - the outlines are drawn as designed.

Needs freetype-py and fontTools on the host (pip install freetype-py
fonttools); without them it says so and skips.
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64

try:
    import freetype
    from fontTools.ttLib import TTFont
except ImportError:
    freetype = None

FONTS = os.path.join(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))), "fonts")

# How close is close enough: a coverage value is a byte, and the two
# renderers cut arcs differently. Measured: at most 4 apart, on average
# under a tenth. The limits leave room for that and no more.
MAX_DIFF = 8
MEAN_DIFF = 0.5

HEADER = re.compile(r"U\+([0-9a-f]+) g=(\d+) adv=(-?\d+) r=(-?\d+) "
                    r"box=(-?\d+),(-?\d+),(\d+),(\d+)")


def parse_sheet(text):
    lines = [l.strip() for l in text.replace("\r", "").split("\n")]
    glyphs, i = {}, 0
    while i < len(lines):
        m = HEADER.search(lines[i])
        i += 1
        if not m:
            continue
        c = int(m.group(1), 16)
        w, h = int(m.group(7)), int(m.group(8))
        rows = []
        for _ in range(h):
            # A line the kernel printed in the middle of the sheet - the
            # network saying it got an address, say - is not a row of
            # the glyph; the serial port is shared
            while i < len(lines) and w and not re.fullmatch(r"[0-9a-f]+",
                                                            lines[i]):
                i += 1
            row = lines[i] if i < len(lines) else ""
            i += 1
            try:
                rows.append(bytes.fromhex(row)[:w])
            except ValueError:
                rows.append(b"")
        glyphs[c] = dict(g=int(m.group(2)), adv=int(m.group(3)),
                         r=int(m.group(4)),
                         box=(int(m.group(5)), int(m.group(6)), w, h),
                         px=rows)
    return glyphs


class Reference:
    def __init__(self, path):
        self.face = freetype.Face(path)
        tt = TTFont(path)
        self.cmap = tt.getBestCmap()
        self.order = tt.getGlyphOrder()
        self.glyphs = tt["maxp"].numGlyphs
        self.upem = tt["head"].unitsPerEm

    def glyph(self, c, px):
        self.face.set_pixel_sizes(0, px)
        self.face.load_char(c, freetype.FT_LOAD_RENDER |
                            freetype.FT_LOAD_NO_HINTING)
        g = self.face.glyph
        b = g.bitmap
        rows = [bytes(b.buffer[r * b.pitch:r * b.pitch + b.width])
                for r in range(b.rows)]
        return g.advance.x, (g.bitmap_left, g.bitmap_top, b.width, b.rows), rows


def compare(label, ours, ref, px, chars):
    gid_bad = adv_bad = box_bad = missing = 0
    worst, worst_c, total, n = 0, None, 0, 0
    for c in chars:
        if c not in ours:
            missing += 1
            continue
        o = ours[c]
        if ref.order[o["g"]] != ref.cmap[c]:
            gid_bad += 1
        adv, box, rows = ref.glyph(c, px)
        if o["adv"] != adv:
            adv_bad += 1
        if o["box"] != box:
            box_bad += 1
            continue
        for ro, rr in zip(o["px"], rows):
            if len(ro) != len(rr):
                box_bad += 1
                break
            for a, b in zip(ro, rr):
                d = abs(a - b)
                total += d
                n += 1
                if d > worst:
                    worst, worst_c = d, c
    mean = total / n if n else 99
    check("%s, %d px: every one of %d characters drawn (%d missing)"
          % (label, px, len(chars), missing), missing == 0)
    check("%s, %d px: the character map picks fontTools' glyph for each "
          "(%d wrong)" % (label, px, gid_bad), gid_bad == 0)
    check("%s, %d px: advances equal to FreeType's to 1/64 px (%d differ)"
          % (label, px, adv_bad), adv_bad == 0)
    check("%s, %d px: bitmap boxes equal to FreeType's (%d differ)"
          % (label, px, box_bad), box_bad == 0)
    check("%s, %d px: coverage within %d of FreeType's - worst %d%s, "
          "mean %.3f over %d pixels"
          % (label, px, MAX_DIFF, worst,
             " (U+%04X)" % worst_c if worst_c else "", mean, n),
          n > 0 and worst <= MAX_DIFF and mean <= MEAN_DIFF)


def sheets(q, sans, mono, tag):
    all_sans = sorted(sans.cmap)
    all_mono = sorted(mono.cmap)
    # Glyphs that are hard in different ways: composites (accents,
    # Polish letters), deep curves, thin strokes, a glyph much wider
    # than tall, and one with holes
    hard = [ord(c) for c in "Ag@&%WQé£ąęłśżŻ€"]

    out = q.command("font", "mono.ttf", timeout=20)
    m = re.search(r"sans\.ttf - (\d+) glyphs, (\d+) units per em", out)
    check("%sthe kernel reads the font's own numbers (%s glyphs, %s units "
          "per em; fontTools: %d, %d)"
          % (tag, m.group(1) if m else "?", m.group(2) if m else "?",
             sans.glyphs, sans.upem),
          m is not None and int(m.group(1)) == sans.glyphs and
          int(m.group(2)) == sans.upem)

    plan = [("sans", sans, 12, all_sans), ("sans", sans, 16, all_sans),
            ("mono", mono, 13, all_mono), ("sans", sans, 48, hard),
            ("sans", sans, 100, hard[:6])]
    if tag:
        plan = plan[1:2] + plan[3:4]        # the other kernel: the essentials
    for name, ref, px, chars in plan:
        text = ""
        # One command per run of characters: a sheet of every character
        # at 48 px would be most of a megabyte of hex for nothing
        if len(chars) <= 24:
            runs = [(c, c) for c in chars]
        else:
            runs, start = [], chars[0]
            for a, b in zip(chars, chars[1:] + [None]):
                if b is None or b != a + 1:
                    runs.append((start, a))
                    start = b
            if len(runs) > 8:
                # the kernel skips what the font does not have, so one
                # range over the gaps costs only the lookups
                runs = [(chars[0], chars[-1])]
        for first, last in runs:
            text += q.command("font sheet %s %d %d %d" % (name, px, first, last),
                              "font sheet:", timeout=180)
        compare(tag + name, parse_sheet(text), ref, px, chars)


def main():
    print()
    print("--- TrueType: the kernel's glyphs against FreeType's ---")
    if freetype is None:
        print("SKIPPED: freetype-py and fontTools are needed to compare "
              "with (pip install freetype-py fonttools)")
        return summary()

    sans = Reference(os.path.join(FONTS, "sans.ttf"))
    mono = Reference(os.path.join(FONTS, "mono.ttf"))

    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
    try:
        q.wait_serial("thread>", timeout=60)
        sheets(q, sans, mono, "")
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
        sheets(q, sans, mono, "(64) ")
    finally:
        q.close()
        for f in (iso, slog):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


sys.exit(main())
