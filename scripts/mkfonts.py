#!/usr/bin/env python3
"""Cuts the fonts ThreadOS carries out of the DejaVu fonts.

    python3 scripts/mkfonts.py [directory with DejaVuSans.ttf]

needs fonttools (pip install fonttools). The results are committed to
fonts/, so building the system does not need this script or fonttools -
only changing which characters the system knows does.

Three of them: the sans for the desktop's text, its bold for titles
and names that have to stand out, and the monospaced one for anything
that wants columns.

What is kept, and why:

  the characters: ASCII, Latin-1 and Latin Extended-A (which is where
  the Polish, Czech, Hungarian ... letters are), and a handful of
  typographic marks - dashes, quotes, the ellipsis, the bullet, the
  euro sign. Everything outside that is a glyph the system cannot type
  anyway.

  the outlines and the metrics: glyf, loca, cmap, hmtx, hhea, head,
  maxp - what kernel/ttf.c reads - and the name, OS/2 and post tables,
  so that the file is still a font any other tool will open.

What goes, and why:

  the hinting instructions. The renderer does not run them - an
  interpreter for TrueType bytecode is a project of its own - and
  carrying hundreds of kilobytes nobody executes is no service.

  GSUB and GPOS: ligatures and kerning. Not used yet; when they are,
  this is the line to change.

The fonts are renamed, as the Bitstream Vera licence asks of a modified
font, though their names never contained the words it reserves.
"""

import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu"

CHARS = (list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) +
         list(range(0x100, 0x180)) +
         [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026,
          0x20AC])

FONTS = [("DejaVuSans.ttf", "sans.ttf", "DejaVu Sans ThreadOS"),
         ("DejaVuSans-Bold.ttf", "bold.ttf", "DejaVu Sans Bold ThreadOS"),
         ("DejaVuSansMono.ttf", "mono.ttf", "DejaVu Sans Mono ThreadOS")]


def make(src, dst, name):
    opts = subset.Options()
    opts.hinting = False
    opts.layout_features = []
    opts.drop_tables += ["GSUB", "GPOS", "GDEF", "kern", "FFTM", "gasp",
                         "prep", "fpgm", "cvt ", "MATH"]
    opts.name_IDs = ["*"]
    opts.notdef_outline = True
    opts.recalc_bounds = True
    font = TTFont(src)
    sub = subset.Subsetter(options=opts)
    sub.populate(unicodes=CHARS)
    sub.subset(font)
    for rec in font["name"].names:
        if rec.nameID in (1, 3, 4, 16):
            family = name if rec.nameID != 4 else name
            rec.string = family
        elif rec.nameID == 6:
            rec.string = name.replace(" ", "")
    font.save(dst)
    return os.path.getsize(dst)


def main():
    os.makedirs(os.path.join(HERE, "fonts"), exist_ok=True)
    for src, dst, name in FONTS:
        out = os.path.join(HERE, "fonts", dst)
        size = make(os.path.join(SOURCE, src), out, name)
        print("%s -> fonts/%s: %d B" % (src, dst, size))


main()
