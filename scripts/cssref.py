#!/usr/bin/env python3
"""Where Chromium puts the boxes of the CSS probes (scripts/tests/cssprobes.py):
the reference t_webcss holds ThreadOS's browser against.

Chromium is a layout engine written apart from litehtml. It is driven
through Playwright (Node.js), which the test machines need not have, so
its answers are kept in scripts/tests/cssref.json; run this again after
changing a probe:

  python3 scripts/cssref.py
"""

import json
import os
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "tests"))
import cssprobes             # noqa: E402

OUT = os.path.join(HERE, "tests", "cssref.json")

JS = r"""
const { chromium } = require('playwright');
const fs = require('fs');
const [pages, dir] = [JSON.parse(fs.readFileSync(process.argv[2])), process.argv[3]];
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage({ viewport: { width: %d, height: %d } });
  for (const [n, html] of Object.entries(pages)) {
    await p.setContent(html);
    await p.screenshot({ path: `${dir}/${n}.png` });
  }
  await b.close();
})();
"""


def png_rgb(path):
    """A Playwright screenshot's pixels: 8-bit RGB or RGBA, no interlace"""
    data = open(path, "rb").read()
    pos, idat, w, h, ch = 8, b"", 0, 0, 0
    while pos < len(data):
        n = int.from_bytes(data[pos:pos + 4], "big")
        kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h = int.from_bytes(body[0:4], "big"), int.from_bytes(body[4:8], "big")
            ch = {2: 3, 6: 4}[body[9]]
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw, rows, prev = zlib.decompress(idat), [], bytearray(w * ch)
    stride = w * ch
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - ch] if i >= ch else 0
            b, c = prev[i], prev[i - ch] if i >= ch else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, lambda x, y: tuple(rows[y][x * ch:x * ch + 3])


def main():
    work = tempfile.mkdtemp()
    pages = {n: cssprobes.page(b).decode() for n, b in cssprobes.PROBES}
    with open(os.path.join(work, "pages.json"), "w") as fh:
        json.dump(pages, fh)
    with open(os.path.join(work, "shot.js"), "w") as fh:
        fh.write(JS % (cssprobes.WIDTH, cssprobes.HEIGHT))
    root = subprocess.run(["npm", "root", "-g"], capture_output=True, text=True).stdout.strip()
    subprocess.run(["node", os.path.join(work, "shot.js"), os.path.join(work, "pages.json"), work],
                   env=dict(os.environ, NODE_PATH=root), check=True)
    out = {}
    for n in pages:
        w, h, at = png_rgb(os.path.join(work, n + ".png"))
        out[n] = cssprobes.boxes(w, h, at)
    with open(OUT, "w") as fh:
        json.dump(out, fh, indent=1, sort_keys=True)
        fh.write("\n")
    for n, b in out.items():
        print(n, b)


if __name__ == "__main__":
    main()
