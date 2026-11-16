#!/usr/bin/env python3
"""ThreadOS smoke test: the Gallery.

The Gallery (user64/ggallery.c) shows the pictures in the Gallery
folder of the home as squares. Pictures made here with Pillow - an
independent PNG, JPEG and GIF writer and reader - are fetched into that
folder over the network, with a text file and a PNG that is not one,
and what the Gallery shows is held against them:

  the four pictures, and not the text file, in name order, each name
  under its square

  a PNG of four coloured quarters, shrunk into its square keeping its
  shape: each quarter its own colour; a JPEG's two halves as Pillow
  decodes them; a GIF smaller than the square, not made larger, in the
  middle of it; the broken one says it cannot be shown

  a click opens one over the window at its own size, named in the bar
  with "4 / 4"; the left arrow goes to the one before; Backspace goes
  back to all of them

  a picture put into the folder while the Gallery is open appears in
  it without anything being done

Run on its own or through scripts/run_tests.py.
"""

import io
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import ptest                 # noqa: E402
from ptest import *          # noqa: F401,F403,E402
from ptest import check, summary, _kernel_define   # noqa: E402

_G = "user64/ggallery.c"
W0 = _kernel_define(_G, "W0")
H0 = _kernel_define(_G, "H0")
PAD = _kernel_define(_G, "PAD")
HEAD_H = _kernel_define(_G, "HEAD_H")
THUMB = _kernel_define(_G, "THUMB")
CELL_W = _kernel_define(_G, "CELL_W")
CELL_H = _kernel_define(_G, "CELL_H")
NAME_Y = _kernel_define(_G, "NAME_Y")
NAME_H = _kernel_define(_G, "NAME_H")
VIEW_BAR_H = _kernel_define(_G, "VIEW_BAR_H")
DARK = theme("dark")

QUAD = [(230, 30, 40), (40, 200, 60), (30, 80, 230), (250, 210, 20)]


def pictures():
    from PIL import Image
    out = {}
    quad = Image.new("RGB", (160, 120))
    for i, c in enumerate(QUAD):
        quad.paste(c, ((i % 2) * 80, (i // 2) * 60, (i % 2) * 80 + 80,
                       (i // 2) * 60 + 60))
    b = io.BytesIO()
    quad.save(b, "PNG")
    out["quad.png"] = b.getvalue()
    photo = Image.new("RGB", (200, 100), (200, 120, 60))
    photo.paste((60, 110, 190), (100, 0, 200, 100))
    b = io.BytesIO()
    photo.save(b, "JPEG", quality=90)
    out["photo.jpg"] = b.getvalue()
    dot = Image.new("P", (64, 64), 0)
    dot.putpalette([200, 40, 200] + [0, 0, 0] * 255)
    b = io.BytesIO()
    dot.save(b, "GIF")
    out["dot.gif"] = b.getvalue()
    out["broken.png"] = b"\x89PNG\r\n\x1a\n" + bytes(range(256)) * 4
    out["notes.txt"] = b"not a picture\n"
    return out


def near(a, b, tol):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def main():
    try:
        from PIL import Image
    except ImportError:
        check("Pillow is there to make the pictures with", False)
        return summary()
    files = pictures()
    for name, data in files.items():
        ptest.SERVED["/" + name] = data
    later = io.BytesIO()
    Image.new("RGB", (40, 40), (10, 180, 170)).save(later, "PNG")
    ptest.SERVED["/zz-new.png"] = later.getvalue()
    pil_photo = Image.open(io.BytesIO(files["photo.jpg"])).convert("RGB")

    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    port, stop_http = start_http_server()
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop())
            W, H = q.size()
            q.mouse_home()
            q.click_desktop()
            home = "/home/admin"
            q.command("mkdir %s/Gallery" % home)
            fetched = 0
            for name in files:
                out = q.command("run httpget 10.0.2.2 %d /%s %s/Gallery/%s"
                                % (port, name, home, name), "has ended",
                                timeout=30)
                fetched += "has ended (code 0)" in out
            check("five files fetched into the Gallery folder (%d)" % fetched,
                  fetched == len(files))

            q.launch_menu("ggallery")
            q.mouse_goto(W - 10, H // 2)
            gx = gy = None
            end = time.time() + 15
            while time.time() < end and gx is None:
                time.sleep(0.5)
                for slot in range(8):
                    x, y = cascade(slot, W0 + 2, H0 + TITLE_H + 2, w=W, h=H)
                    if q.window_title(x, y, W0 + 2) == "Gallery":
                        gx, gy = x + 1, y + CONTENT_DY
                        break
            check("the Gallery opens from the menu", gx is not None)
            if gx is None:
                return summary()

            cols = (W0 - 2 * PAD) // CELL_W
            x0 = gx + (W0 - cols * CELL_W) // 2

            def square(i):
                return (x0 + (i % cols) * CELL_W + (CELL_W - THUMB) // 2,
                        gy + HEAD_H + (i // cols) * CELL_H)

            def name_of(i, shot=None):
                cx = x0 + (i % cols) * CELL_W
                _, sy = square(i)
                return q.find_ui(cx + 2, cx + CELL_W - 2,
                                 ui_font_baseline_in(11, sy + NAME_Y, NAME_H),
                                 px=11, fg=DARK["app_text"], screen=shot)

            want = sorted(n for n in files if not n.endswith(".txt"))
            names = []
            end = time.time() + 20
            while time.time() < end and names != want:
                time.sleep(0.5)
                shot = q.screen_raw()
                names = [name_of(i, shot) for i in range(len(want))]
            check("the four pictures, not the text file, in name order (%s)"
                  % names, names == want)

            # The squares: shrunk once each, a pass at a time
            time.sleep(3)
            _, _, at = q.screen_raw()
            sx, sy = square(want.index("quad.png"))
            tw, th = THUMB, 120 * THUMB // 160
            tx, ty = sx, sy + (THUMB - th) // 2
            got = [at(tx + tw // 4 + (i % 2) * tw // 2,
                      ty + th // 4 + (i // 2) * th // 2) for i in range(4)]
            check("the PNG's four quarters, each its colour in the square "
                  "(%s)" % got, got == QUAD)
            sx, sy = square(want.index("photo.jpg"))
            tw, th = THUMB, 100 * THUMB // 200
            ty = sy + (THUMB - th) // 2
            left, right = at(sx + tw // 4, ty + th // 2), \
                at(sx + 3 * tw // 4, ty + th // 2)
            ref_l, ref_r = pil_photo.getpixel((50, 50)), \
                pil_photo.getpixel((150, 50))
            check("the JPEG's two halves as Pillow decodes them (%s %s / "
                  "%s %s)" % (left, right, ref_l, ref_r),
                  near(left, ref_l, 6) and near(right, ref_r, 6))
            sx, sy = square(want.index("dot.gif"))
            inside = at(sx + THUMB // 2, sy + THUMB // 2)
            edge_in = at(sx + (THUMB - 64) // 2 + 1, sy + THUMB // 2)
            edge_out = at(sx + (THUMB - 64) // 2 - 3, sy + THUMB // 2)
            check("the GIF, smaller than the square, at its own size in the "
                  "middle (%s, %s, %s)" % (inside, edge_in, edge_out),
                  inside == (200, 40, 200) and edge_in == (200, 40, 200) and
                  edge_out != (200, 40, 200))
            sx, sy = square(want.index("broken.png"))
            said = q.find_ui(sx, sx + THUMB,
                             ui_font_baseline_in(11, sy, THUMB), px=11,
                             fg=DARK["app_dim"])
            check("the broken PNG says so ('%s')" % said,
                  said == "cannot be shown")

            # One picture over the window
            sx, sy = square(want.index("quad.png"))
            q.click(sx + THUMB // 2, sy + THUMB // 2)
            q.mouse_goto(W - 10, H // 2)
            bar = gy + H0 - VIEW_BAR_H
            vh = H0 - VIEW_BAR_H
            vx, vy = gx + (W0 - 160) // 2, gy + (vh - 120) // 2

            def bar_text(shot=None):
                return q.find_ui(gx + W0 // 3, gx + W0 - 4,
                                 ui_font_baseline_in(UD_PX, bar, VIEW_BAR_H),
                                 px=UD_PX, screen=shot,
                                 colours=[DARK["app_text"], DARK["app_dim"]])
            seen = ""
            end = time.time() + 10
            while time.time() < end and "quad.png" not in seen:
                time.sleep(0.5)
                seen = bar_text()
            _, _, at = q.screen_raw()
            got = [at(vx + 40 + (i % 2) * 80, vy + 30 + (i // 2) * 60)
                   for i in range(4)]
            which = q.find_ui(gx + W0 - 80, gx + W0 - 4,
                              ui_font_baseline_in(UD_PX, bar, VIEW_BAR_H),
                              px=UD_PX, fg=DARK["app_dim"])
            check("a click opens it at its own size, its quarters where "
                  "they are (%s), named in the bar ('%s', '%s')"
                  % (got, seen, which),
                  got == QUAD and "quad.png" in seen and which == "4 / 4")
            q.sendkey("left")
            seen = ""
            end = time.time() + 10
            while time.time() < end and "photo.jpg" not in seen:
                time.sleep(0.5)
                seen = bar_text()
            check("the left arrow: the one before ('%s')" % seen,
                  "photo.jpg" in seen)
            q.sendkey("backspace")
            names = []
            end = time.time() + 10
            while time.time() < end and names != want:
                time.sleep(0.5)
                shot = q.screen_raw()
                names = [name_of(i, shot) for i in range(len(want))]
            check("Backspace: all of them again", names == want)

            # A picture arriving while it is open
            q.click_desktop()
            q.command("run httpget 10.0.2.2 %d /zz-new.png %s/Gallery/"
                      "zz-new.png" % (port, home), "has ended", timeout=30)
            q.mouse_goto(W - 10, H // 2)
            got = ""
            end = time.time() + 15
            while time.time() < end and got != "zz-new.png":
                time.sleep(0.5)
                got = name_of(len(want))
            check("a picture put into the folder appears by itself ('%s')"
                  % got, got == "zz-new.png")
        finally:
            q.close()
    finally:
        stop_http()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


sys.exit(main())
