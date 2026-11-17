#!/usr/bin/env python3
"""Turns ThreadOS's mark (art/threados-mark.svg) into what the kernel draws.

    mklogo.py SVG OUT.h SIZE

The kernel shows the mark (kernel/mark.c) on the loading screen, the
menu bar, the taskbar's Start and the About box, and has no SVG reader: this reads the paths and polygons of
a one-colour SVG - the commands M L H V C S Z, absolute and relative -
flattens the curves and fills the shapes by the non-zero rule into a
picture of coverage, cut to the mark (not the viewBox) and SIZE pixels
on its longer side. Each pixel is cut
into 16 rows, and along each row the shapes' exact span is taken, so
an edge's pixel gets as much as the shape covers of it. The kernel
scales that down to the screen's size, averaging.

Writes LOGO_W, LOGO_H and logo_alpha[] (0-255, row by row) as a C
header. t_bootsplash holds what comes out on the screen to FreeType's
drawing of the same SVG, read by fontTools' own parser.
"""

import re
import sys
import xml.etree.ElementTree as ET

SUB = 16                     # rows of samples to a pixel
CURVE_STEPS = 24             # straight pieces to a cubic


def tokens(d):
    return re.findall(r"[MmLlHhVvCcSsZz]|[-+]?(?:\d+\.\d*|\.\d+|\d+)"
                      r"(?:[eE][-+]?\d+)?", d)


def cubic(p0, p1, p2, p3):
    out = []
    for i in range(1, CURVE_STEPS + 1):
        t = i / CURVE_STEPS
        u = 1 - t
        out.append((u * u * u * p0[0] + 3 * u * u * t * p1[0] +
                    3 * u * t * t * p2[0] + t * t * t * p3[0],
                    u * u * u * p0[1] + 3 * u * u * t * p1[1] +
                    3 * u * t * t * p2[1] + t * t * t * p3[1]))
    return out


def path_polygons(d):
    """A path's subpaths as lists of points, curves flattened"""
    tok = tokens(d)
    polys, cur = [], []
    x = y = sx = sy = 0.0
    ctrl = None                      # the last cubic's second control point
    cmd = None
    i = 0

    def num():
        nonlocal i
        v = float(tok[i])
        i += 1
        return v
    while i < len(tok):
        if re.match(r"[A-Za-z]", tok[i]):
            cmd = tok[i]
            i += 1
            if cmd in "Zz":
                if cur:
                    polys.append(cur)
                cur, x, y, ctrl = [], sx, sy, None
                continue
        rel = cmd.islower()
        c = cmd.upper()
        if c == "M":
            nx, ny = num(), num()
            if rel:
                nx, ny = x + nx, y + ny
            if cur:
                polys.append(cur)
            x, y, sx, sy = nx, ny, nx, ny
            cur = [(x, y)]
            cmd = "l" if rel else "L"        # further pairs are lines
            ctrl = None
        elif c == "L":
            nx, ny = num(), num()
            x, y = (x + nx, y + ny) if rel else (nx, ny)
            cur.append((x, y))
            ctrl = None
        elif c == "H":
            nx = num()
            x = x + nx if rel else nx
            cur.append((x, y))
            ctrl = None
        elif c == "V":
            ny = num()
            y = y + ny if rel else ny
            cur.append((x, y))
            ctrl = None
        elif c in "CS":
            if c == "C":
                x1, y1 = num(), num()
                if rel:
                    x1, y1 = x + x1, y + y1
            else:                            # the reflection of the last one
                x1, y1 = (2 * x - ctrl[0], 2 * y - ctrl[1]) if ctrl else (x, y)
            x2, y2, nx, ny = num(), num(), num(), num()
            if rel:
                x2, y2, nx, ny = x + x2, y + y2, x + nx, y + ny
            cur += cubic((x, y), (x1, y1), (x2, y2), (nx, ny))
            ctrl = (x2, y2)
            x, y = nx, ny
        else:
            raise SystemExit("mklogo: the path command %s is not known" % cmd)
    if cur:
        polys.append(cur)
    return polys


def shapes(svg):
    root = ET.parse(svg).getroot()
    vb = [float(v) for v in root.get("viewBox").replace(",", " ").split()]
    polys = []
    for el in root.iter():
        tag = el.tag.split("}")[-1]
        if tag == "path":
            polys += path_polygons(el.get("d"))
        elif tag == "polygon":
            v = [float(t) for t in re.findall(r"[-+]?(?:\d+\.\d*|\.\d+|\d+)",
                                              el.get("points"))]
            polys.append(list(zip(v[0::2], v[1::2])))
    return vb, polys


def rasterize(vb, polys, size):
    """Coverage of the shapes, cut to the mark itself: the SVG's viewBox
    has margins of its own, and a mark that fills its box is one the
    callers can size and centre as they mean to"""
    xs = [x for p in polys for x, _ in p]
    ys = [y for p in polys for _, y in p]
    vx, vy = min(xs), min(ys)
    vw, vh = max(xs) - vx, max(ys) - vy
    s = size / max(vw, vh)
    w, h = round(vw * s), round(vh * s)
    sx, sy = w / vw, h / vh
    edges = []
    for p in polys:
        pts = [((x - vx) * sx, (y - vy) * sy) for x, y in p]
        for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1]):
            if y0 != y1:
                edges.append((x0, y0, x1, y1))
    cover = [[0.0] * w for _ in range(h)]
    for row in range(h * SUB):
        y = (row + 0.5) / SUB
        cross = []
        for x0, y0, x1, y1 in edges:
            if (y0 <= y < y1) or (y1 <= y < y0):
                cross.append((x0 + (y - y0) * (x1 - x0) / (y1 - y0),
                              1 if y1 > y0 else -1))
        cross.sort()
        line = cover[row // SUB]
        wind = 0
        for k, (xa, d) in enumerate(cross):
            wind += d
            if not wind or k + 1 == len(cross):
                continue
            xb = cross[k + 1][0]
            a, b = max(0.0, xa), min(float(w), xb)
            px = int(a)
            while px < b:
                line[px] += (min(b, px + 1) - max(a, px)) / SUB
                px += 1
    return w, h, [min(255, int(c * 255 + 0.5)) for r in cover for c in r]


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 1
    vb, polys = shapes(sys.argv[1])
    w, h, alpha = rasterize(vb, polys, int(sys.argv[3]))
    with open(sys.argv[2], "w") as f:
        f.write("/* Made by scripts/mklogo.py from %s - do not edit */\n"
                % sys.argv[1])
        f.write("#define LOGO_W %d\n#define LOGO_H %d\n" % (w, h))
        f.write("static const uint8_t logo_alpha[LOGO_W * LOGO_H] = {\n")
        for r in range(h):
            f.write("    " + ",".join(str(v) for v in alpha[r * w:(r + 1) * w])
                    + ",\n")
        f.write("};\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
