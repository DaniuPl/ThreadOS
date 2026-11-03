#!/usr/bin/env python3
"""ThreadOS test: WebP pictures - the decoder, and the browser showing them.

On the host first: user/uwebp.h, compiled as it is with the address and
undefined-behaviour sanitizers, against libwebp - Google's own decoder,
through Pillow - on 86 pictures libwebp wrote (scripts/tests/webpcases.py):
lossy at four qualities, three efforts and four sizes, lossless at three
efforts, palettes of 2 to 200 colours, alpha kept exactly, lossy with
alpha, and an animation. Each must come out the same to the last byte -
lossy ones too: VP8's decoding is exact, and the colour conversion is
the one libwebp uses.

Then in QEMU: a page with a lossy picture, a palette one and one with
alpha, each held pixel by pixel against libwebp's reading of it, laid on
the page's white.

Run on its own or through scripts/run_tests.py.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from webtest import *        # noqa: F401,F403
from webtest import check, run, visit
import webpcases             # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

TOOL = r"""
#include <stdio.h>
#include <stdlib.h>
#include "uwebp.h"
int main(int argc, char **argv)
{
    (void)argc;
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(n ? n : 1);
    if (fread(b, 1, n, f) != (size_t)n)
        return 2;
    fclose(f);
    unsigned char *px;
    int w, h;
    const char *why;
    if (webp_decode(b, n, &px, &w, &h, &why) < 0) {
        printf("error %s\n", why);
        return 1;
    }
    FILE *o = fopen(argv[2], "wb");
    fwrite(px, 4, (size_t)w * h, o);
    fclose(o);
    printf("%d %d\n", w, h);
    free(px);
    free(b);
    return 0;
}
"""


def host(cases):
    work = tempfile.mkdtemp()
    try:
        src = os.path.join(work, "tool.c")
        open(src, "w").write(TOOL)
        tool = os.path.join(work, "tool")
        out = subprocess.run(["gcc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra",
                              "-fsanitize=address,undefined",
                              "-fno-sanitize-recover=all", "-I",
                              os.path.join(ROOT, "user"), "-o", tool, src],
                             capture_output=True, text=True)
        check("the decoder builds on the host, with the sanitizers",
              out.returncode == 0)
        if out.returncode:
            print(out.stderr[:2000])
            return
        wrong = []
        for name, data, (w, h, ref) in cases:
            pic, raw = os.path.join(work, "p.webp"), os.path.join(work, "p.rgba")
            open(pic, "wb").write(data)
            r = subprocess.run([tool, pic, raw], capture_output=True, text=True,
                               timeout=60)
            got = open(raw, "rb").read() if r.returncode == 0 else b""
            if r.stdout.split() != [str(w), str(h)] or got != ref:
                wrong.append(f"{name}: {r.stdout.strip()} {r.stderr[-200:]}")
        kinds = {k: sum(1 for n, _, _ in cases if n.split(",")[0].split(" ")[0] == k
                        and ("with alpha" in n) == (k == "alpha"))
                 for k in ("lossy", "lossless", "an")}
        kinds["alpha"] = sum(1 for n, _, _ in cases if "with alpha" in n)
        check(f"all {len(cases)} pictures come out as libwebp reads them, to "
              f"the byte ({kinds}; wrong: {wrong[:3]})", not wrong)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def on_white(w, h, rgba):
    """As the browser lays a picture on the page: (c*a + 255*(255-a)) / 255"""
    out = []
    for i in range(w * h):
        a = rgba[i * 4 + 3]
        out.append(tuple((rgba[i * 4 + k] * a + 255 * (255 - a)) // 255
                         for k in range(3)))
    return out


def browser(cases):
    pick = {"lossy": "lossy 333x211 quality 90 method 4",
            "palette": "lossless, a palette of 12",
            "alpha": "lossy with alpha, alpha quality 100"}
    by_name = {n: (d, r) for n, d, r in cases}
    frames = {"lossy": "#123456", "palette": "#654321", "alpha": "#345612"}
    body = "".join(
        f'<div style="display:inline-block;vertical-align:top;border:2px solid '
        f'{frames[k]};margin:2px"><img src="/{k}.webp" style="display:block"></div>'
        for k in pick)
    page = (f'<html><head><title>WebP</title></head><body style="margin:4px">'
            f'{body}</body></html>').encode()

    def section(e):
        q = e.q
        SERVED["/webp.html"] = page
        for k, n in pick.items():
            SERVED[f"/{k}.webp"] = by_name[n][0]
        got = visit(e, "/webp.html", "Web: WebP")
        check(f"the page with the WebP pictures opens ({got!r})", got == "Web: WebP")
        time.sleep(2)
        for k, n in pick.items():
            w, h, ref = by_name[n][1]
            col = tuple(int(frames[k][i:i + 2], 16) for i in (1, 3, 5))
            box, _ = pixels(q, *e.page, col)
            inner = box and (box[0] + 2, box[1] + 2, box[2] - 1, box[3] - 1)
            size = inner and (inner[2] - inner[0], inner[3] - inner[1])
            if size != (w, h):
                check(f"{n}: shown at its size, {w}x{h} ({size})", False)
                continue
            _, _, at = q.screen_raw()
            want = on_white(w, h, ref)
            worst = max(max(abs(a - b) for a, b in zip(at(inner[0] + x, inner[1] + y),
                                                     want[y * w + x]))
                        for y in range(h) for x in range(w))
            check(f"{n}: on the screen as libwebp reads it ({w}x{h}, the "
                  f"largest difference {worst})", worst == 0)
    return section


def main():
    print()
    print("--- WebP pictures ---")
    if not webpcases.available():
        check("Pillow with WebP is here to compare with (it is not: skipped)", True)
        return summary()
    cases = webpcases.cases()
    host(cases)
    return run("WebP pictures in the browser", [browser(cases)])


sys.exit(main())
