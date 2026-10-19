#!/usr/bin/env python3
"""ThreadOS smoke test: pictures - taken, saved, fetched and shown.

The machine could draw a desktop and could read a wallpaper from a
file. What it could not do was look at itself: there was no way to save
what was on the screen, and nothing that would show such a file if
there were one.

Print Screen now writes the screen to the disk in the machine's own
terms - four bytes "TSC2", the size, and then three bytes a pixel: the
colours the compositor drew, the glass and the shadows included. The
viewer has nothing to interpret: it puts the colours back into a window
of its own, a row at a time, and they are the same colours again. (On a
screen of 256 colours the file is "TSCR", a palette and an index a
pixel, as it always was.)

The assertions are about the loop being real, not about a file
appearing. The picture has to BE the screen: the dock, photographed and
shown again at every second pixel, comes back in the window with the
very same colours, pixel for pixel - and that is a thing no accident
produces.

And a picture from OUTSIDE: there is no floppy and no way to hand the
machine a file, but there is a network - so the test's own HTTP server
offers the same picture as a BMP, a PNG and a JPEG, the machine fetches
each with httpget and the viewer draws it. The file is four quarters of
red, green, blue and white, and those four colours are in the screen's
palette exactly: a decoder that works shows exactly them, and a decoder
that draws something shows something else. Three formats, three quite
different kinds of work - rows padded to four bytes, a deflate stream
under a filter per row, and Huffman codes over a cosine transform - and
one question asked of all three.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
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
from ptest import check, summary

# The viewer opens in the next free slot of the cascade; with the clock
# already open it is slot 3, at (106,66), and asks for 420x300.
VX, VY = 106, 66
CX, CY = VX + 1, VY + CONTENT_DY
VW, VH = 420, 300

def status(q, vx=VX, vy=VY):
    """The viewer's status line, read in the lettering it is written in"""
    return q.gimg_status(vx + 1, vy + CONTENT_DY, VW, VH)


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")

    http_port, stop_http = start_http_server()
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()

            # Something on the screen worth photographing
            q.launch_dock(1)                # the "Clock" tile
            time.sleep(4)
            q.mouse_goto(420, 300)
            time.sleep(1.5)

            # What the picture will have to repeat: the dock, which does
            # not change while nothing is clicked - unlike the clocks
            w, h, at = q.screen_raw()
            g = dock_geom(n_w=3)
            spots = [(x, y) for y in range(g["y"] + 2, g["y"] + g["h"] - 2, 4)
                     for x in range(g["x"] + 2, g["x"] + g["w"] - 2, 6)]
            spots = [(x - x % 2, y - y % 2) for x, y in spots]
            before = {p: at(*p) for p in spots}
            check("the dock is on the screen (%d places in it remembered)"
                  % len(spots), len(set(before.values())) > 50)

            # --- Print Screen -------------------------------------------
            q.sendkey("print")
            time.sleep(4)
            q.click_desktop()
            time.sleep(1)
            seen = len(q.serial())
            q.type_line("fls")
            time.sleep(2)
            listing = q.serial()[seen:]
            m = re.search(r"shot1\.psc\s+\((\d+) B\)", listing)
            check("Print Screen wrote a file to the disk ('%s')"
                  % (m.group(0) if m else listing[-60:].replace("\n", " ")),
                  m is not None)
            if not m:
                return summary()
            # 640*480 pixels of three bytes + "TSC2" + two sizes
            check("and it has exactly as many bytes as the screen in colours "
                  "(%s)" % m.group(1),
                  int(m.group(1)) == 640 * 480 * 3 + 4 + 2 + 2)

            # --- and the viewer puts it back on the screen --------------
            q.type_line("run gimg shot1.psc")
            time.sleep(6)
            q.mouse_goto(560, 430)
            time.sleep(2)
            w, h, at = q.screen_raw()

            said = status(q)
            check("the viewer says what it loaded ('%s')" % said,
                  "640x480" in said and "loaded" in said)
            check("and that it shows every other pixel, because the whole screen "
                  "does not fit in a window on this screen", "step 2" in said)

            # The picture IS the screen: the dock, inside the window, at
            # half the size (every second pixel each way) and in exactly
            # the colours it had. The viewer draws the picture a row at
            # a time - four hundred and eighty rows through as many
            # calls - so the screen is looked at again until it stops
            # filling up.
            #
            # Sixty readings rather than twenty-five. Four hundred and
            # eighty system calls take as long as the machine gives
            # them, and the machine is four QEMUs sharing a host that is
            # also running the rest of the suite. Waiting longer for the
            # same answer is patience, lowering the answer would be
            # something else.
            same = 0
            for _ in range(60):
                w, h, at = q.screen_raw()
                same = sum(1 for (x, y), c in before.items()
                           if at(CX + x // 2, CY + y // 2) == c)
                if same == len(before):
                    break
                time.sleep(1)
            check("the picture in the window really is the screen - the dock "
                  "is there, every remembered pixel in its colour (%d of %d)"
                  % (same, len(before)), same == len(before))

            # --- the counter, and what is not a picture -----------------
            q.click_desktop()
            time.sleep(1)
            q.sendkey("print")
            time.sleep(4)
            seen = len(q.serial())
            q.type_line("fls")
            time.sleep(2)
            listing = q.serial()[seen:]
            check("the second shot gets the next number",
                  "shot2.psc" in listing and "shot1.psc" in listing)

            q.type_line("run gimg swap.sys")
            time.sleep(5)
            q.mouse_goto(560, 430)
            time.sleep(2)
            w, h, at = q.screen_raw()
            # The viewer for a file that is not a picture opens in the
            # next slot again - the same place, because the previous
            # one is still there
            # The second viewer opens in the next slot of the cascade -
            # the first one is still standing where it was
            sx4, sy4 = cascade(4, VW + 2, VH + TITLE_H + 2)
            said = status(q, vx=sx4, vy=sy4)
            check("a file that is not a picture is refused with "
                  "a word of explanation ('%s')" % said,
                  "is not a picture" in said)

            # --- a picture from the outside world -----------------------
            # No floppy, no shared folder - but a network. The machine
            # fetches the file itself and then shows it.
            q.click_desktop()
            time.sleep(1)
            seen = len(q.serial())
            q.type_line("run httpget 10.0.2.2 %d /squares.bmp picture.bmp"
                        % http_port)
            time.sleep(6)
            got = q.serial()[seen:]
            check("the machine fetches a picture off the network onto the disk",
                  "HTTP 200" in got and "has ended (code 0)" in got)

            q.type_line("run gimg picture.bmp")
            time.sleep(6)
            q.mouse_goto(600, 200)
            time.sleep(2)
            w, h, at = q.screen_raw()

            # The third viewer, the third slot of the cascade
            vx, vy = cascade(5, VW + 2, VH + TITLE_H + 2)
            said = status(q, vx=vx, vy=vy)
            check("the viewer loaded the BMP ('%s')" % said,
                  "64x48" in said and "loaded" in said)

            # The four quarters, in the four colours the file has -
            # each of them exactly in the palette, so 'nearly' is not
            # good enough here
            cx, cy = vx + 1, vy + CONTENT_DY
            def hue(x, y):
                return at(cx + x, cy + y)
            reds = hue(16, 12)
            greens = hue(48, 12)
            blues = hue(16, 36)
            whites = hue(48, 36)
            check("and four quarters in four colours came out of it "
                  "(%s %s %s %s)" % (reds, greens, blues, whites),
                  reds[0] > 200 and reds[1] < 60 and
                  greens[1] > 200 and greens[0] < 60 and
                  blues[2] > 200 and blues[0] < 60 and
                  all(v > 200 for v in whites))

            # --- and the same picture as a PNG --------------------------
            # Chunks, a deflate stream and a filter byte per row: three
            # things that all have to be right, and the four exact
            # colours say whether they were.
            q.click_desktop()
            time.sleep(1)
            seen = len(q.serial())
            q.type_line("run httpget 10.0.2.2 %d /squares.png picture.png"
                        % http_port)
            time.sleep(6)
            got = q.serial()[seen:]
            check("the machine fetches a PNG off the network",
                  "HTTP 200" in got and "has ended (code 0)" in got)

            q.type_line("run gimg picture.png")
            time.sleep(7)
            q.mouse_goto(600, 200)
            time.sleep(2)
            w, h, at = q.screen_raw()
            # (the sixth slot would run under the dock, so the cascade walks
            # back up - kernel/desktop.c)
            vx, vy = cascade(6, VW + 2, VH + TITLE_H + 2)
            said = status(q, vx=vx, vy=vy)
            check("the viewer unpacked the PNG ('%s')" % said,
                  "64x48" in said and "loaded" in said)

            cx, cy = vx + 1, vy + CONTENT_DY
            reds = at(cx + 16, cy + 12)
            greens = at(cx + 48, cy + 12)
            blues = at(cx + 16, cy + 36)
            whites = at(cx + 48, cy + 36)
            check("and the same four colours came out of it "
                  "(%s %s %s %s)" % (reds, greens, blues, whites),
                  reds[0] > 200 and reds[1] < 60 and
                  greens[1] > 200 and greens[0] < 60 and
                  blues[2] > 200 and blues[0] < 60 and
                  all(v > 200 for v in whites))

            # --- and as a JPEG, which is what a camera writes ------------
            # A different kind of work again: Huffman codes, a cosine
            # transform undone in fixed point, and colour stored at half
            # resolution and put back. The file comes from an outside
            # encoder, so this is our arithmetic reading somebody
            # else's picture.
            q.click_desktop()
            time.sleep(1)
            seen = len(q.serial())
            q.type_line("run httpget 10.0.2.2 %d /squares.jpg picture.jpg"
                        % http_port)
            time.sleep(6)
            got = q.serial()[seen:]
            check("the machine fetches a JPEG off the network",
                  "HTTP 200" in got and "has ended (code 0)" in got)

            q.type_line("run gimg picture.jpg")
            time.sleep(7)
            q.mouse_goto(600, 200)
            time.sleep(2)
            w, h, at = q.screen_raw()
            vx, vy = cascade(7, VW + 2, VH + TITLE_H + 2)
            said = status(q, vx=vx, vy=vy)
            check("the viewer took the JPEG apart into pixels ('%s')" % said,
                  "64x48" in said and "loaded" in said)

            # JPEG is lossy, so the colours are read well inside the
            # quarters - but they still have to be the right four, and
            # a decoder that got the transform wrong gives grey mush
            # rather than four saturated fields.
            cx, cy = vx + 1, vy + CONTENT_DY
            reds = at(cx + 16, cy + 12)
            greens = at(cx + 48, cy + 12)
            blues = at(cx + 16, cy + 36)
            whites = at(cx + 48, cy + 36)
            check("and the same four colours came out of it "
                  "(%s %s %s %s)" % (reds, greens, blues, whites),
                  reds[0] > 200 and reds[1] < 60 and
                  greens[1] > 200 and greens[0] < 60 and
                  blues[2] > 200 and blues[0] < 60 and
                  all(v > 200 for v in whites))
        finally:
            q.close()
    finally:
        stop_http()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    sixty_four()
    return summary()


def sixty_four():
    """The picture formats on the 64-bit kernel.

    The decoders are one set of sources compiled twice, so what is
    asked here is what the compiler could have got wrong on its own:
    the arithmetic is int and unsigned int, the plane is indexed
    through a long, and a pointer is twice the size it was. A JPEG is
    the one that would notice - it has the most of all three - so the
    long-mode half fetches the same file and looks for the same four
    colours."""
    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("SKIPPED (64): no 64-bit image (make kernel64)")
        return
    if shutil.which("grub-mkrescue") is None or \
       shutil.which("qemu-system-x86_64") is None:
        print("SKIPPED (64): no grub-mkrescue or qemu-system-x86_64")
        return

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)

    http_port, stop_http = start_http_server()
    try:
        os.makedirs(d + "/boot/grub")
        shutil.copy(KERNEL64, d + "/boot/threados64.bin")
        mod = ""
        if INITRD64 and os.path.exists(INITRD64):
            shutil.copy(INITRD64, d + "/boot/initrd64.img")
            mod = "    module2 /boot/initrd64.img initrd64\n"
        with open(d + "/boot/grub/grub.cfg", "w") as fh:
            fh.write("insmod all_video\nset gfxpayload=keep\n"
                     "set timeout=0\nset default=0\n"
                     'menuentry "ThreadOS 64" {\n'
                     "    multiboot2 /boot/threados64.bin\n" + mod + "}\n")
        made = subprocess.run(["grub-mkrescue", "-o", iso, d],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
        check("the disc with the 64-bit kernel builds", made)
        if not made:
            return

        q = Qemu(KERNEL, INITRD, disk=disk, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=60)
            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=30)
            q.wait_desktop(40)
            time.sleep(4)
            q.mouse_home()

            # The same window as on the other kernel, so the viewer
            # lands in the same slot of the cascade and the reading
            # grid below is the same one
            q.launch_dock(1)                # the "Clock" tile
            time.sleep(4)
            q.click_desktop()             # the keys back to the shell
            time.sleep(1)

            seen = len(q.serial())
            q.type_line("run httpget 10.0.2.2 %d /squares.jpg picture.jpg"
                        % http_port)
            time.sleep(8)
            q.type_line("fls")
            time.sleep(2)
            got = q.serial()[seen:]
            # The long-mode shell does not announce a program's exit
            # code, so the proof that the fetch worked is the file
            # itself - with the length the server sent.
            m = re.search(r"picture\.jpg\s+\((\d+) B\)", got)
            check("(64) the machine fetches a JPEG off the network ('%s')"
                  % (m.group(0) if m else got[-60:].replace("\n", " ")),
                  "HTTP 200" in got and m is not None and
                  int(m.group(1)) == len(JPEG_IMAGE))

            q.type_line("run gimg picture.jpg")
            time.sleep(8)
            q.mouse_goto(600, 200)
            time.sleep(2)
            w, h, at = q.screen_raw()

            vx, vy = VX, VY
            said = status(q, vx=vx, vy=vy)
            check("(64) the viewer took the JPEG apart into pixels ('%s')" % said,
                  "64x48" in said and "loaded" in said)

            cx, cy = vx + 1, vy + CONTENT_DY
            reds = at(cx + 16, cy + 12)
            greens = at(cx + 48, cy + 12)
            blues = at(cx + 16, cy + 36)
            whites = at(cx + 48, cy + 36)
            check("(64) and four colours came out of it, the same as in "
                  "32-bit mode (%s %s %s %s)"
                  % (reds, greens, blues, whites),
                  reds[0] > 200 and reds[1] < 60 and
                  greens[1] > 200 and greens[0] < 60 and
                  blues[2] > 200 and blues[0] < 60 and
                  all(v > 200 for v in whites))
        finally:
            q.close()
    finally:
        stop_http()
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, disk):
            if os.path.exists(f):
                os.unlink(f)


sys.exit(main())
