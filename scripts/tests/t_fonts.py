#!/usr/bin/env python3
"""ThreadOS smoke test: FreeType and HarfBuzz.

FreeType 2.15.0 (its release archive) and HarfBuzz 15.1.0 (its src/
directory) are built against musl. fonttest shapes five texts with
HarfBuzz - which glyphs, at which cluster, with which advance and
offset - and draws every glyph with FreeType, unhinted and with light
hinting, summing the pixels. Two fonts:

  DejaVu Sans, whole, from this machine: TrueType outlines, its own
  kerning (GPOS) and its own ligatures, Polish letters and combining
  marks

  a font this test makes with fontTools: CFF outlines - which the
  kernel's own TrueType rasteriser cannot read - with a kerning pair and
  a ligature whose effect is known in advance: A V moves 300 units
  closer, f i becomes one glyph

The same fonttest, built on this machine with the same FreeType and
HarfBuzz, has to print the same lines; the made-up font's kerning and
ligature are checked by their numbers as well.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROMPT = "thread64>"
DEJAVU = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"


def makefile_list(name):
    with open(os.path.join(ROOT, "Makefile")) as fh:
        text = fh.read().replace("\\\n", " ")
    return re.search(r"^%s\s*:=\s*(.*)$" % name, text, re.M).group(1).split()


def make_cff_font(path):
    """Upright rectangles in CFF outlines: 'A' and 'V' 600 units wide,
    'f' and 'i' 400, the 'f_i' ligature 900, a kerning pair A V of -300"""
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.t2CharStringPen import T2CharStringPen
    from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
    names = [".notdef", "space", "A", "V", "f", "i", "f_i"]
    widths = {".notdef": 500, "space": 300, "A": 600, "V": 600, "f": 400,
              "i": 400, "f_i": 900}
    fb = FontBuilder(1000, isTTF=False)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({0x20: "space", 0x41: "A", 0x56: "V", 0x66: "f",
                          0x69: "i"})
    charstrings = {}
    for n in names:
        pen = T2CharStringPen(widths[n], None)
        if n != "space":
            w = widths[n]
            pen.moveTo((50, 0))
            pen.lineTo((50, 700))
            pen.lineTo((w - 50, 700))
            pen.lineTo((w - 50, 0))
            pen.closePath()
        charstrings[n] = pen.getCharString()
    fb.setupCFF("ThreadOSTestCFF", {"FullName": "ThreadOS Test CFF"},
                charstrings, {})
    fb.setupHorizontalMetrics({n: (widths[n], 50) for n in names})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": "ThreadOS Test CFF",
                       "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=800, usWinAscent=800, usWinDescent=200)
    fb.setupPost()
    addOpenTypeFeaturesFromString(fb.font, """
        languagesystem DFLT dflt;
        languagesystem latn dflt;
        feature liga { sub f i by f_i; } liga;
        feature kern { pos A V -300; } kern;
    """)
    fb.save(path)


def host_run(work, fonts):
    """fonttest with the same FreeType and HarfBuzz, built here"""
    ft = os.path.join(work, "freetype")
    os.makedirs(ft)
    subprocess.run(["tar", "-xf", os.path.join(ROOT, "third_party",
                                               "freetype-2.15.0.tar.xz"),
                    "-C", ft, "--strip-components=1"], check=True)
    objs = []
    for src in makefile_list("FT_SRC"):
        o = os.path.join(work, src.replace("/", "-") + ".o")
        r = subprocess.run(["gcc", "-O2", "-w", "-DFT2_BUILD_LIBRARY",
                            "-DFT_CONFIG_MODULES_H=<ftmodule.h>",
                            "-DFT_CONFIG_OPTION_SYSTEM_ZLIB",
                            "-I" + os.path.join(ROOT, "apps", "fonts"),
                            "-I" + os.path.join(ft, "include"), "-c",
                            os.path.join(ft, "src", src + ".c"), "-o", o])
        if r.returncode:
            return None
        objs.append(o)
    hb = os.path.join(work, "harfbuzz.o")
    hbsrc = os.path.join(ROOT, "third_party", "harfbuzz", "src")
    if subprocess.run(["g++", "-O1", "-w", "-fno-exceptions", "-fno-rtti",
                       "-DHAVE_FREETYPE=1", "-I" + os.path.join(ft, "include"),
                       "-c", os.path.join(hbsrc, "harfbuzz.cc"), "-o",
                       hb]).returncode:
        return None
    exe = os.path.join(work, "fonttest")
    if subprocess.run(["g++", "-O2", "-w", "-x", "c",
                       "-I" + os.path.join(ft, "include"), "-I" + hbsrc,
                       os.path.join(ROOT, "libc", "tests", "fonttest.c"),
                       "-x", "none", hb] + objs + ["-lz", "-o", exe]
                      ).returncode:
        return None
    run = os.path.join(work, "hostrun")
    os.makedirs(run)
    for f in fonts:
        shutil.copy(f, run)
    p = subprocess.run([exe] + [os.path.basename(f) for f in fonts], cwd=run,
                       capture_output=True, text=True, timeout=60)
    return p.stdout.splitlines()


def run(q, command, timeout):
    seen = len(q.serial())
    q.type_line(command)
    name = command.split()[1]
    deadline = time.time() + timeout
    while time.time() < deadline:
        if f"[{name}] done, exit code" in q.serial()[seen:]:
            break
        time.sleep(0.5)
    out = q.serial()[seen:].encode("latin1").decode("utf-8", "replace")
    m = re.search(r"\[%s\] done, exit code (-?\d+)" % re.escape(name), out)
    lines = [l.strip("\r") for l in out.replace("\r", "").split("\n")]
    body = [l for l in lines[1:] if l and not l.startswith(
            (f"[{name}]", "[kernel]", "[ OK ]", "[ !! ]", PROMPT))]
    return body, int(m.group(1)) if m else None


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    if not os.path.exists(DEJAVU):
        print("SKIPPED: no DejaVu Sans on this machine (fonts-dejavu-core)")
        return summary()
    work = tempfile.mkdtemp()
    http_port, http_stop = start_http_server()
    try:
        cff = os.path.join(work, "test.otf")
        make_cff_font(cff)
        dejavu = os.path.join(work, "dejavu.ttf")
        shutil.copy(DEJAVU, dejavu)
        want = host_run(work, [dejavu, cff])
        check("the same FreeType and HarfBuzz build and run on this machine",
              want is not None)
        if want is None:
            return summary()
        for name in ("dejavu.ttf", "test.otf"):
            with open(os.path.join(work, name), "rb") as fh:
                SERVED["/" + name] = fh.read()

        # The made-up font's features, by their numbers: A V kerned to
        # 600 - 300 units, f i one glyph of 900 units (at 20 px, in 64ths of
        # a pixel: 384 for the kerned A, 768 for V, 1152 for f_i)
        cff_lines = want[want.index(next(l for l in want
                                          if l.startswith("test.otf"))):]
        kern = next((l for l in cff_lines if l.startswith("  [1]")), "")
        liga = next((l for l in cff_lines if l.startswith("  [2]")), "")
        check(f"(on the host: A V kerned by 300 units - '{kern[:60]}')",
              re.search(r" 2@0\+384 3@1\+768", kern) is not None)
        check(f"(and f i one glyph - '{liga[:60]}')",
              re.search(r" 6@\d+\+1152", liga) is not None)

        iso = os.path.join(work, "t.iso")
        disk = os.path.join(work, "disk.img")
        with open(disk, "wb") as fh:
            fh.write(b"\0" * DISK_SIZE)
        check("the disc with the kernel builds",
              make_iso64(os.path.join(work, "iso"), iso))
        q = Qemu(KERNEL, INITRD, cdrom=iso, disk=disk, sata=True,
                 serial=os.path.join(work, "serial.log"),
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial(PROMPT, timeout=90)
            q.wait_serial("] Disk ", timeout=30)
            q.wait_serial("[net] DHCP", timeout=60)
            for name in ("dejavu.ttf", "test.otf"):
                _, code = run(q, f"run httpget 10.0.2.2 {http_port} /{name} "
                              f"{name}", 120)
                check(f"{name} reaches the ThreadOS disk", code == 0)
            got, code = run(q, "run fonttest dejavu.ttf test.otf", 300)
        finally:
            q.close()
        first = next((i for i, (w, g) in enumerate(zip(want, got)) if w != g),
                     None)
        where = (f"first difference: host {want[first]!r}, ThreadOS "
                 f"{got[first]!r}" if first is not None else
                 f"host {len(want)} lines, ThreadOS {len(got)}")
        check(f"fonttest prints exactly what the host's build prints ({where})",
              got == want)
        check(f"and ends with code 0 (got {code})", code == 0)
        check("(the comparison covers both fonts and every text: "
              f"{len(want)} lines)",
              len(want) == 1 + 2 * (1 + 5 * 2) + 1)
    finally:
        http_stop()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
