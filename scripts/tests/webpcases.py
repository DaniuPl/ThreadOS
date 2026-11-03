"""WebP pictures for the tests, written by libwebp (through Pillow) -
not by us - with what libwebp reads back from each

Lossy ones at four qualities, three encoder efforts and four sizes (one
of a single pixel, ones not a whole number of macroblocks); lossless
ones at three efforts, palettes of 2 to 200 colours (2, 4 and 16 pack
several pixels into a byte), alpha kept exactly; lossy with alpha, its
alpha lossless or not; and an animation, of which the first frame is
what shows.
"""

import io
import random


def _picture(w, h, seed, alpha=False):
    from PIL import Image, ImageDraw, ImageFilter
    rnd = random.Random(seed)
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = (x * 255 // max(1, w - 1), y * 255 // max(1, h - 1),
                        ((x + y) * 3) & 255)
    d = ImageDraw.Draw(im)
    for _ in range(12):
        x0, y0 = rnd.randrange(w), rnd.randrange(h)
        d.ellipse([x0, y0, x0 + rnd.randrange(1, w // 2 + 2),
                   y0 + rnd.randrange(1, h // 2 + 2)],
                  fill=(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)))
    for _ in range(w * h // 20):                 # noise: hard for an encoder
        px[rnd.randrange(w), rnd.randrange(h)] = (rnd.randrange(256),) * 3
    im = im.filter(ImageFilter.SMOOTH)
    if alpha:
        a = Image.new("L", (w, h))
        ap = a.load()
        for y in range(h):
            for x in range(w):
                ap[x, y] = 0 if (x // 7 + y // 5) % 4 == 0 else min(255, (x * 4 + y * 2) % 300)
        im = im.convert("RGBA")
        im.putalpha(a)
    return im


def _palette(w, h, n, seed):
    from PIL import Image
    rnd = random.Random(seed)
    cols = [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256))
            for _ in range(n)]
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = cols[((x // 3) * 7 + (y // 2) * 3) % n]
    return im


def available():
    try:
        from PIL import features
        return bool(features.check("webp"))
    except ImportError:
        return False


def cases():
    """(name, file bytes, (w, h, RGBA bytes libwebp decodes))"""
    from PIL import Image
    pics = []
    for (w, h) in [(1, 1), (17, 13), (64, 48), (333, 211)]:
        for q in (5, 50, 90, 100):
            for m in (0, 4, 6):
                pics.append((f"lossy {w}x{h} quality {q} method {m}",
                             _picture(w, h, w * q + m), dict(quality=q, method=m)))
    for (w, h) in [(1, 1), (40, 30), (257, 99)]:
        for q in (0, 50, 100):
            for m in (0, 3, 6):
                pics.append((f"lossless {w}x{h} quality {q} method {m}",
                             _picture(w, h, w + q + m),
                             dict(lossless=True, quality=q, method=m)))
    for n in (2, 3, 4, 12, 16, 17, 200):
        pics.append((f"lossless, a palette of {n}", _palette(61, 37, n, n),
                     dict(lossless=True)))
    pics.append(("lossless, alpha exact", _picture(90, 70, 3, True),
                 dict(lossless=True, exact=True)))
    for aq in (100, 30):
        pics.append((f"lossy with alpha, alpha quality {aq}",
                     _picture(90, 70, 4, True), dict(quality=70, alpha_quality=aq)))
    out = []
    for name, im, opts in pics:
        f = io.BytesIO()
        im.save(f, "WEBP", **opts)
        data = f.getvalue()
        back = Image.open(io.BytesIO(data)).convert("RGBA")
        out.append((name, data, (back.width, back.height, back.tobytes())))
    # An animation: two frames; the first is the picture
    f = io.BytesIO()
    a, b = _picture(48, 32, 7), _picture(48, 32, 8)
    a.save(f, "WEBP", save_all=True, append_images=[b], duration=100,
           lossless=True)
    data = f.getvalue()
    back = Image.open(io.BytesIO(data))
    back.seek(0)
    back = back.convert("RGBA")
    out.append(("an animation's first frame", data,
                (back.width, back.height, back.tobytes())))
    return out
