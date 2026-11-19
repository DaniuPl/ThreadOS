#!/usr/bin/env python3
"""ThreadOS smoke test: libpng and libjpeg-turbo.

The image decoders a web page's pictures will go through, built from
their releases for ThreadOS (Makefile, 'Libraries for WebKit's
processes'). libc/tests/imgtest.c decodes the files it is given to
8-bit RGBA and says, of each, its size, a hash of every pixel and three
pixels; this test makes the files here and works the same out with
another decoder, Pillow, or straight from the pixels it wrote:

  PNG: RGB, RGBA, a palette with a transparent entry, grey, grey with
  alpha, 16-bit grey, and 16-bit RGBA interlaced (Adam7), which this
  file encodes itself, Pillow writing neither

  JPEG: baseline 4:2:0, progressive, 4:4:4, grey - pixel for pixel what
  Pillow's libjpeg-turbo makes of them

  a PNG cut short, a PNG with a byte changed under its CRC, and a file
  that only calls itself a JPEG: an error, said as such - where Pillow
  fails too

  and imgtest's own: PNG there and back to the last bit, JPEG at quality
  90 within a few levels

Run on its own or through scripts/run_tests.py.
"""

import io
import os
import re
import shutil
import struct
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import ptest                 # noqa: E402
from ptest import *          # noqa: F401,F403,E402
from ptest import check, summary  # noqa: E402

PROMPTS = ("thread64>", "thread64#")


def fnv(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def describe(name, w, h, rgba):
    def px(i):
        return rgba[i * 4:i * 4 + 4].hex()
    return (f"{name}: {w}x{h} pixels {fnv(rgba):08x} first {px(0)} middle "
            f"{px((h // 2) * w + w // 2)} last {px(w * h - 1)}")


def picture(w, h, seed):
    """An RGBA picture with gradients, edges and noise-like detail"""
    out = bytearray()
    for y in range(h):
        for x in range(w):
            out += bytes(((x * 255 // (w - 1) + seed) & 255,
                          (y * 255 // (h - 1)) & 255,
                          ((x * 7) ^ (y * 13) ^ seed) & 255,
                          (255 - (x * y + seed) % 256) & 255))
    return bytes(out)


def png_chunk(kind, data):
    return (struct.pack(">I", len(data)) + kind + data +
            struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))


def png16_interlaced(w, h, rgba16):
    """A 16-bit RGBA PNG with Adam7 interlacing, written here: neither
    Pillow nor anything else used by this test writes one"""
    passes = ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4),
              (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2))
    raw = bytearray()
    for x0, y0, dx, dy in passes:
        for y in range(y0, h, dy):
            xs = range(x0, w, dx)
            if not len(xs):
                continue
            raw.append(0)                      # no filter
            for x in xs:
                raw += struct.pack(">4H", *rgba16[(y * w + x) * 4:
                                                  (y * w + x) * 4 + 4])
    ihdr = struct.pack(">IIBBBBB", w, h, 16, 6, 0, 0, 1)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr) +
            png_chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
            png_chunk(b"IEND", b""))


def make_files():
    """{name: (bytes, the line imgtest should print)} - the lines from
    Pillow's decoding, or from the pixels themselves"""
    from PIL import Image
    files = {}
    w, h = 97, 61
    src = picture(w, h, 0)
    rgba = Image.frombytes("RGBA", (w, h), src)

    def pillow_line(name, data):
        img = Image.open(io.BytesIO(data))
        img.load()
        return describe(name, img.width, img.height,
                        img.convert("RGBA").tobytes())

    def keep(name, img, fmt, **kw):
        buf = io.BytesIO()
        img.save(buf, fmt, **kw)
        data = buf.getvalue()
        files[name] = (data, pillow_line(name, data))

    keep("rgb.png", rgba.convert("RGB"), "PNG")
    keep("rgba.png", rgba, "PNG")
    pal = rgba.convert("RGB").quantize(64)
    keep("palette.png", pal, "PNG", transparency=5)
    keep("grey.png", rgba.convert("L"), "PNG")
    keep("greya.png", rgba.convert("LA"), "PNG")

    # 16-bit grey: Pillow reads it as it is; imgtest cuts each value to
    # its high byte (png_set_strip_16), so that is what is wanted
    vals = [(x * 1031 + y * 977) & 0xFFFF for y in range(h) for x in range(w)]
    g16 = Image.frombytes("I;16", (w, h), struct.pack("<%dH" % len(vals), *vals))
    buf = io.BytesIO()
    g16.save(buf, "PNG")
    data = buf.getvalue()
    back = Image.open(io.BytesIO(data))
    # Pillow before 10.3 (Ubuntu's 10.2, on CI) opens 16-bit grey as mode
    # "I", 32 bits a pixel, and has no packer from it to "I;16"; later
    # ones open it as "I;16". The values are the same either way.
    if back.mode == "I;16":
        got = list(struct.unpack("<%dH" % (w * h),
                                 back.tobytes("raw", "I;16")))
    else:
        got = list(struct.unpack("<%di" % (w * h),
                                 back.tobytes("raw", "I;32S")))
    want = bytearray()
    for v in got:
        want += bytes((v >> 8, v >> 8, v >> 8, 255))
    files["grey16.png"] = (data, describe("grey16.png", w, h, bytes(want)))

    rgba16 = [(c * 257 + (i * 37) % 251) & 0xFFFF
              for i, c in enumerate(src)]
    data = png16_interlaced(w, h, rgba16)
    want = bytes(v >> 8 for v in rgba16)
    files["rgba16i.png"] = (data, describe("rgba16i.png", w, h, want))

    photo = rgba.convert("RGB")
    keep("base.jpg", photo, "JPEG", quality=85, subsampling=2)
    keep("prog.jpg", photo, "JPEG", quality=80, progressive=True)
    keep("full.jpg", photo, "JPEG", quality=95, subsampling=0)
    keep("grey.jpg", photo.convert("L"), "JPEG", quality=85)

    good = files["rgb.png"][0]
    broken = {
        "cut.png": good[:len(good) // 2],
        "crc.png": good[:60] + bytes([good[60] ^ 0x40]) + good[61:],
        "not.jpg": b"\xff\xd8\xff\xe0 this is not really a JPEG at all",
    }
    for name, data in broken.items():
        try:
            img = Image.open(io.BytesIO(data))
            img.load()
            pillow = "Pillow read it"
        except Exception:
            pillow = "Pillow failed"
        files[name] = (data, f"{name}: an error, said as such", pillow)
    return files


def run(q, command, timeout=120):
    seen = len(q.serial())
    q.type_line(command)
    name = command.split()[1]
    end = time.time() + timeout
    while time.time() < end and f"[{name}] done, exit code" not in \
            q.serial()[seen:]:
        time.sleep(0.5)
    out = q.serial()[seen:]
    start = out.find(f"Program '{name}' from initrd")
    out = out[start:] if start >= 0 else out
    m = re.search(r"\[%s\] done, exit code (-?\d+)" % re.escape(name), out)
    body = [l.strip() for l in out.replace("\r", "").split("\n")[1:]
            if l.strip() and not l.strip().startswith(("[",) + PROMPTS)]
    return body, int(m.group(1)) if m else None


def main():
    print("--- libpng and libjpeg-turbo ---")
    try:
        import PIL  # noqa: F401
    except ImportError:
        print("SKIPPED: no Pillow to make the pictures and read them")
        return summary()
    files = make_files()
    for broken in ("cut.png", "crc.png", "not.jpg"):
        check(f"(Pillow fails on {broken} too: {files[broken][2]})",
              files[broken][2] == "Pillow failed")
    for name, entry in files.items():
        ptest.SERVED["/" + name] = entry[0]
    work = tempfile.mkdtemp()
    disk = os.path.join(work, "disk.img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    port, stop_http = start_http_server()
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=os.path.join(work, "s.log"))
        try:
            check("the desktop came up", q.wait_desktop())
            q.click_desktop()
            fetched = 0
            for name in files:
                _, c = run(q, f"run httpget 10.0.2.2 {port} /{name} {name}")
                fetched += c == 0
            check(f"the pictures reach the disk ({fetched} of {len(files)})",
                  fetched == len(files))
            got, code = run(q, "run imgtest " + " ".join(files))
            lines = {l.split(":")[0]: l for l in got if not l.startswith("~")}
            for name, entry in files.items():
                check(f"{name}: {entry[1].split(': ', 1)[1]}"
                      if lines.get(name) == entry[1] else
                      f"{name}: imgtest {lines.get(name)!r}, "
                      f"want {entry[1]!r}", lines.get(name) == entry[1])
            for l in got:
                if l.endswith((": yes", ": no")):
                    check(f"imgtest: {l}", l.endswith(": yes"))
                elif l.startswith("~"):
                    print("   " + l)
            check(f"and imgtest ends with code 0 (got {code})", code == 0)
        finally:
            q.close()
    finally:
        stop_http()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
