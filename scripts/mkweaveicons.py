#!/usr/bin/env python3
"""Turns Weave's icons (docs/weave/canvas/Icon.dc.html) into what the
kernel draws.

    mkweaveicons.py ICON.dc.html OUT.h

Weave's icons (docs/WEAVE.md) are a tile of one dye with a few white
strokes on it, drawn on a grid of 48: the design keeps them as SVG path
data in a table of its own (Icon.dc.html, `const I = {...}`), and that
table is what this reads, so that the design and the system cannot
drift apart. The kernel has no SVG reader. Every path is flattened here
- the commands M L H V C S Q T A Z, absolute and relative, arcs by the
SVG specification's own conversion (its appendix F.6.5) - into broken
lines in sixteenths of a unit of the grid, which kernel/weave.c strokes
at any size with round ends and joins, as the design's
stroke-linecap="round" stroke-linejoin="round" ask. A filled path (the
design's F(...)) is a dot: it is written as a disc, its centre and
radius, and anything else filled is refused, so that a new kind of
shape in the design stops the build instead of coming out wrong.

The design has no waste bin, which the desktop shows; one is added here
in the same hand (TRASH below). And the loom's small pictures - the
network, the sound, the magnifier of the field that finds anything -
come from Main.dc.html's own SVG, and loom view's lock, restart and
shut down from Spool.dc.html's, on a grid of 24 (GLYPHS below, doubled
onto the icons' 48): strokes only, no tile, named "g-...".

Writes WV_ICON_N, the points, the lines and the discs, and wv_icons[]:
each icon's name, dye and where its lines and discs begin.
"""

import math
import re
import sys

UNIT = 16                    # the grid's unit, in what the header stores
TOLERANCE = 0.04             # how far a flattened curve may stray, in units

# Not in the design: the desktop's waste bin, drawn as its icons are
TRASH = ("trash", "#4B505E", [
    ("S", "M13 16h22"),
    ("S", "M20 16v-2.5a1.5 1.5 0 0 1 1.5-1.5h5a1.5 1.5 0 0 1 1.5 1.5V16"),
    ("S", "M16 16l1.6 18.2a2 2 0 0 0 2 1.8h8.8a2 2 0 0 0 2-1.8L32 16"),
    ("S", "M21.5 21v10M26.5 21v10"),
])


# The loom's pictures, as Main.dc.html draws them (viewBox 0 0 24 24)
GLYPHS = [
    ("g-network", [("S", "M3 9a14 14 0 0 1 18 0"),
                   ("S", "M6 12.5a9 9 0 0 1 12 0"),
                   ("S", "M9 16a4 4 0 0 1 6 0"),
                   ("F", "M12 19m-1 0a1 1 0 1 0 2 0a1 1 0 1 0-2 0")]),
    ("g-sound", [("S", "M4 10v4h4l5 4V6L8 10z"),
                 ("S", "M16.5 9a4 4 0 0 1 0 6")]),
    ("g-find", [("S", "M11 11m-6 0a6 6 0 1 0 12 0a6 6 0 1 0-12 0"),
                ("S", "M20 20l-4.5-4.5")]),
    # loom view's (Spool.dc.html): lock, restart, shut down
    ("g-lock", [("S", "M7 11h10a2 2 0 0 1 2 2v5a2 2 0 0 1-2 2H7a2 2 0 0 1"
                      "-2-2v-5a2 2 0 0 1 2-2z"),
                ("S", "M8 11V8a4 4 0 0 1 8 0v3")]),
    ("g-restart", [("S", "M19 12a7 7 0 1 1-2.1-5"), ("S", "M19 4v4h-4")]),
    ("g-power", [("S", "M12 4v8"), ("S", "M7 7a7 7 0 1 0 10 0")]),
]


def tokens(d):
    return re.findall(r"[MmLlHhVvCcSsQqTtAaZz]|[-+]?(?:\d+\.\d*|\.\d+|\d+)"
                      r"(?:[eE][-+]?\d+)?", d)


def arc_points(x1, y1, rx, ry, phi, large, sweep, x2, y2):
    """The SVG specification's endpoint-to-centre conversion (F.6.5),
    then the arc cut into pieces no longer than the tolerance allows"""
    if (x1, y1) == (x2, y2):
        return []
    if rx == 0 or ry == 0:
        return [(x2, y2)]
    rx, ry = abs(rx), abs(ry)
    c, s = math.cos(math.radians(phi)), math.sin(math.radians(phi))
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = c * dx + s * dy, -s * dx + c * dy
    lam = (x1p / rx) ** 2 + (y1p / ry) ** 2
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    k = math.sqrt(max(0.0, num / den))
    if large == sweep:
        k = -k
    cxp, cyp = k * rx * y1p / ry, -k * ry * x1p / rx
    cx = c * cxp - s * cyp + (x1 + x2) / 2
    cy = s * cxp + c * cyp + (y1 + y2) / 2

    def angle(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    t1 = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = angle((x1p - cxp) / rx, (y1p - cyp) / ry,
               (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    r = max(rx, ry)
    step = 2 * math.acos(max(-1.0, 1 - TOLERANCE / r)) if r > TOLERANCE \
        else math.pi / 2
    n = max(2, int(math.ceil(abs(dt) / step)))
    out = []
    for i in range(1, n + 1):
        t = t1 + dt * i / n
        ex, ey = rx * math.cos(t), ry * math.sin(t)
        out.append((c * ex - s * ey + cx, s * ex + c * ey + cy))
    out[-1] = (x2, y2)
    return out


def cubic_points(p0, p1, p2, p3):
    length = sum(math.dist(a, b) for a, b in ((p0, p1), (p1, p2), (p2, p3)))
    n = max(2, int(math.ceil(math.sqrt(length / (8 * TOLERANCE)) * 2)))
    out = []
    for i in range(1, n + 1):
        t = i / n
        u = 1 - t
        out.append((u ** 3 * p0[0] + 3 * u * u * t * p1[0] +
                    3 * u * t * t * p2[0] + t ** 3 * p3[0],
                    u ** 3 * p0[1] + 3 * u * u * t * p1[1] +
                    3 * u * t * t * p2[1] + t ** 3 * p3[1]))
    return out


def flatten(d):
    """A path's data as broken lines: a list of lists of (x, y)"""
    tok = tokens(d)
    lines, cur = [], []
    x = y = sx = sy = 0.0
    last_ctrl = None
    cmd = None
    i = 0

    def num():
        nonlocal i
        v = float(tok[i])
        i += 1
        return v

    def flag():
        # arcs' flags may be written run together ("a3 3 0 1 0 6 0" or
        # "a3 3 0 106 0"): one digit each
        nonlocal i
        t = tok[i]
        if t in ("0", "1"):
            i += 1
            return int(t)
        tok[i] = t[1:]
        return int(t[0])

    while i < len(tok):
        if re.match(r"[A-Za-z]", tok[i]):
            cmd = tok[i]
            i += 1
            if cmd in "Zz":
                if cur:
                    cur.append((sx, sy))
                    lines.append(cur)
                cur = []
                x, y = sx, sy
                last_ctrl = None
                continue
        rel = cmd.islower()
        c = cmd.upper()
        ox, oy = (x, y) if rel else (0.0, 0.0)
        if c == "M":
            if cur and len(cur) > 1:
                lines.append(cur)
            x, y = num() + ox, num() + oy
            sx, sy = x, y
            cur = [(x, y)]
            cmd = "l" if rel else "L"
            last_ctrl = None
            continue
        if not cur:
            cur = [(x, y)]
        if c == "L":
            x, y = num() + ox, num() + oy
            cur.append((x, y))
            last_ctrl = None
        elif c == "H":
            x = num() + ox
            cur.append((x, y))
            last_ctrl = None
        elif c == "V":
            y = num() + (y if rel else 0.0)
            cur.append((x, y))
            last_ctrl = None
        elif c in "CS":
            if c == "C":
                p1 = (num() + ox, num() + oy)
            else:
                p1 = (2 * x - last_ctrl[0], 2 * y - last_ctrl[1]) \
                    if last_ctrl else (x, y)
            p2 = (num() + ox, num() + oy)
            p3 = (num() + ox, num() + oy)
            cur += cubic_points((x, y), p1, p2, p3)
            last_ctrl = p2
            x, y = p3
        elif c in "QT":
            if c == "Q":
                q = (num() + ox, num() + oy)
            else:
                q = (2 * x - last_ctrl[0], 2 * y - last_ctrl[1]) \
                    if last_ctrl else (x, y)
            p3 = (num() + ox, num() + oy)
            p1 = (x + 2 / 3 * (q[0] - x), y + 2 / 3 * (q[1] - y))
            p2 = (p3[0] + 2 / 3 * (q[0] - p3[0]), p3[1] + 2 / 3 * (q[1] - p3[1]))
            cur += cubic_points((x, y), p1, p2, p3)
            last_ctrl = q
            x, y = p3
        elif c == "A":
            rx, ry, phi = num(), num(), num()
            large, sweep = flag(), flag()
            x2, y2 = num() + ox, num() + oy
            cur += arc_points(x, y, rx, ry, phi, large, sweep, x2, y2)
            x, y = x2, y2
            last_ctrl = None
        else:
            raise ValueError("path command %r" % cmd)
    if cur and len(cur) > 1:
        lines.append(cur)
    return lines


def as_disc(d):
    """A filled path, which in these icons is always a dot: its centre and
    radius, or an error"""
    pts = [p for line in flatten(d) for p in line]
    cx = (min(p[0] for p in pts) + max(p[0] for p in pts)) / 2
    cy = (min(p[1] for p in pts) + max(p[1] for p in pts)) / 2
    rs = [math.dist(p, (cx, cy)) for p in pts]
    r = sum(rs) / len(rs)
    if max(abs(v - r) for v in rs) > 0.05 * r + 0.02:
        raise ValueError("a filled shape that is not a dot: %s" % d)
    return cx, cy, r


def read_design(path):
    src = open(path, encoding="utf-8").read()
    body = src[src.index("const I = {"):src.index("};", src.index("const I = {"))]
    icons = []
    for m in re.finditer(r"^\s*(\w+): \['(#[0-9A-Fa-f]{6})', '[^']*', \[(.*)\]\],?\s*$",
                         body, re.M):
        parts = re.findall(r"([SF])\('([^']*)'\)", m.group(3))
        if not parts:
            raise ValueError("an icon with no strokes: %s" % m.group(1))
        icons.append((m.group(1), m.group(2), parts))
    if len(icons) < 20:
        raise ValueError("the icons' table was not read (%d icons)"
                         % len(icons))
    return icons


def q(v):
    return int(round(v * UNIT))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    icons = [(n, d, p, 1) for n, d, p in read_design(sys.argv[1]) + [TRASH]]
    icons += [(n, "#000000", p, 2) for n, p in GLYPHS]
    pts, runs, discs, table = [], [], [], []
    for name, dye, parts, scale in icons:
        run0, pt0, disc0 = len(runs), len(pts) // 2, len(discs) // 3
        for kind, d in parts:
            if kind == "F":
                cx, cy, r = as_disc(d)
                discs += [q(cx * scale), q(cy * scale), q(r * scale)]
                continue
            for line in flatten(d):
                line = [(x * scale, y * scale) for x, y in line]
                kept = [line[0]]
                for p in line[1:]:
                    if (q(p[0]), q(p[1])) != (q(kept[-1][0]), q(kept[-1][1])):
                        kept.append(p)
                if len(kept) == 1:      # a dot drawn as a line of no length
                    kept.append(kept[0])
                runs.append(len(kept))
                for p in kept:
                    pts += [q(p[0]), q(p[1])]
        table.append((name, int(dye[1:], 16), run0, len(runs) - run0, pt0,
                      disc0, len(discs) // 3 - disc0))

    def rows(values, per=12):
        return ",\n".join("    " + ", ".join(str(v) for v in values[i:i + per])
                          for i in range(0, len(values), per))

    with open(sys.argv[2], "w") as out:
        out.write("/* Weave's icons, from docs/weave/canvas/Icon.dc.html - made by\n"
                  "   scripts/mkweaveicons.py; do not edit. Points in sixteenths of\n"
                  "   a unit of the 48-unit grid. */\n")
        out.write("#define WV_ICON_N %d\n" % len(table))
        out.write("#define WV_UNIT %d\n" % UNIT)
        out.write("static const int16_t wv_pts[] = {\n%s\n};\n" % rows(pts))
        out.write("static const uint16_t wv_runs[] = {\n%s\n};\n" % rows(runs))
        out.write("static const int16_t wv_discs[] = {\n%s\n};\n"
                  % (rows(discs) if discs else "    0"))
        out.write("static const struct wv_icon wv_icons[WV_ICON_N] = {\n")
        for name, dye, run0, nruns, pt0, disc0, ndiscs in table:
            out.write('    { "%s", 0x%06X, %d, %d, %d, %d, %d },\n'
                      % (name, dye, run0, nruns, pt0, disc0, ndiscs))
        out.write("};\n")


if __name__ == "__main__":
    main()
