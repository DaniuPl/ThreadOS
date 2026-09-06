#!/usr/bin/env python3
"""ProtonOS smoke test: pictures - taken, saved, fetched and shown.

The machine could draw a desktop and could read a wallpaper from a
file. What it could not do was look at itself: there was no way to save
what was on the screen, and nothing that would show such a file if
there were one.

Print Screen now writes the screen to the disk in the machine's own
terms - four bytes "PSCR", the size, the whole palette (so the file
says what its numbers mean) and then one byte per pixel, the palette
index. That is exactly what the compositor drew, so the viewer has
nothing to interpret: it puts the bytes back into a window through
win_blit, a row at a time, and they are the same colours again.

The assertions are about the loop being real, not about a file
appearing. The picture has to BE the screen: the orange of the icons
covers a quarter of the area inside the window that it covers on the
screen itself, because the picture is shown every second pixel - and
that is a number no accident produces.

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
from ptest import check, summary, font8x8

# The viewer opens in the next free slot of the cascade; with the clock
# already open it is slot 3, at (106,66), and asks for 420x300.
VX, VY = 106, 66
CX, CY = VX + 1, VY + 11
VW, VH = 420, 300
STATUS_H = 10

ORANGE = (255, 162, 0)          # COL_ICON - the icon tiles


def is_orange(px):
    return all(abs(px[i] - ORANGE[i]) <= 30 for i in range(3))


def status(at, cols=48, vx=VX, vy=VY):
    """The viewer's status line: white on the accent blue."""
    font = font8x8()
    x0, y0 = vx + 1 + 4, vy + 11 + VH - STATUS_H + 1
    out = ""
    for c in range(cols):
        pattern = []
        for y in range(8):
            bits = 0
            for x in range(8):
                p = at(x0 + c * 8 + x, y0 + y)
                if all(v > 200 for v in p):
                    bits |= 1 << x
            pattern.append(bits)
        out += font.get(tuple(pattern), " " if not any(pattern) else "?")
    return out.strip()


def count_orange(at, x0, y0, x1, y1):
    return sum(1 for y in range(y0, y1, 2) for x in range(x0, x1, 2)
               if is_orange(at(x, y)))


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
            q.mouse_goto(24, 72)               # the "Clock" icon
            q.mouse_click()
            time.sleep(4)
            q.mouse_goto(420, 300)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            # How much orange the icons put on the screen - the number
            # the picture will have to repeat, a quarter of it
            on_screen = count_orange(at, 0, 0, 64, h - 20)
            check("the icons are on the screen (%d orange samples)"
                  % on_screen, on_screen > 50)

            # --- Print Screen -------------------------------------------
            q.sendkey("print")
            time.sleep(4)
            q.mouse_goto(600, 430)
            q.mouse_click()                    # the keys back to the shell
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
            # 640*480 pixels + "PSCR" + two sizes + 256 colours
            check("and it has exactly as many bytes as the screen plus the palette "
                  "(%s)" % m.group(1),
                  int(m.group(1)) == 640 * 480 + 4 + 2 + 2 + 256 * 3)

            # --- and the viewer puts it back on the screen --------------
            q.type_line("run gimg shot1.psc")
            time.sleep(6)
            q.mouse_goto(560, 430)
            time.sleep(2)
            w, h, at = q.screen_raw()

            said = status(at)
            check("the viewer says what it loaded ('%s')" % said,
                  "640x480" in said and "loaded" in said)
            check("and that it shows every other pixel, because the whole screen "
                  "does not fit in a window on this screen", "co 2" in said)

            # The picture IS the screen: the icons' orange inside the
            # window, in the strip where the icons were, and a quarter
            # of what it was (every second pixel in each direction).
            # The viewer draws the picture a row at a time - four
            # hundred and eighty rows through as many calls - so the
            # screen is looked at again until it stops filling up.
            #
            # Sixty readings rather than twenty-five, and the threshold
            # untouched. Four hundred and eighty system calls take as
            # long as the machine gives them, and the machine is four
            # QEMUs sharing a host that is also running the rest of the
            # suite; the group has grown neighbours twice since this
            # loop was written, and once the whole minute went by with
            # nothing drawn at all. Waiting longer for the same answer
            # is patience, lowering the answer would be something else.
            inside = 0
            for _ in range(60):
                w, h, at = q.screen_raw()
                now = count_orange(at, CX, CY, CX + 32, CY + (h - 20) // 2)
                if now > inside:
                    inside = now
                if inside > on_screen // 6:
                    break
                time.sleep(1)
            check("the picture in the window really is the screen - the icons are in their "
                  "place (%d against %d on the screen)" % (inside, on_screen),
                  inside > on_screen // 6)

            # --- the counter, and what is not a picture -----------------
            q.mouse_goto(600, 430)
            q.mouse_click()
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
            said = status(at, vx=VX + 12, vy=VY + 12)
            check("a file that is not a picture is refused with "
                  "a word of explanation ('%s')" % said,
                  "is not a picture" in said)

            # --- a picture from the outside world -----------------------
            # No floppy, no shared folder - but a network. The machine
            # fetches the file itself and then shows it.
            q.mouse_goto(600, 430)
            q.mouse_click()
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
            vx, vy = VX + 24, VY + 24
            said = status(at, vx=vx, vy=vy)
            check("the viewer loaded the BMP ('%s')" % said,
                  "64x48" in said and "loaded" in said)

            # The four quarters, in the four colours the file has -
            # each of them exactly in the palette, so 'nearly' is not
            # good enough here
            cx, cy = vx + 1, vy + 11
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
            q.mouse_goto(600, 430)
            q.mouse_click()
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
            vx, vy = VX + 36, VY + 36
            said = status(at, vx=vx, vy=vy)
            check("the viewer unpacked the PNG ('%s')" % said,
                  "64x48" in said and "loaded" in said)

            cx, cy = vx + 1, vy + 11
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
            q.mouse_goto(600, 430)
            q.mouse_click()
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
            vx, vy = VX + 48, VY + 48
            said = status(at, vx=vx, vy=vy)
            check("the viewer took the JPEG apart into pixels ('%s')" % said,
                  "64x48" in said and "loaded" in said)

            # JPEG is lossy, so the colours are read well inside the
            # quarters - but they still have to be the right four, and
            # a decoder that got the transform wrong gives grey mush
            # rather than four saturated fields.
            cx, cy = vx + 1, vy + 11
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
        shutil.copy(KERNEL64, d + "/boot/protonos64.bin")
        mod = ""
        if INITRD64 and os.path.exists(INITRD64):
            shutil.copy(INITRD64, d + "/boot/initrd64.img")
            mod = "    module2 /boot/initrd64.img initrd64\n"
        with open(d + "/boot/grub/grub.cfg", "w") as fh:
            fh.write("insmod all_video\nset gfxpayload=keep\n"
                     "set timeout=0\nset default=0\n"
                     'menuentry "ProtonOS 64" {\n'
                     "    multiboot2 /boot/protonos64.bin\n" + mod + "}\n")
        made = subprocess.run(["grub-mkrescue", "-o", iso, d],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
        check("the disc with the 64-bit kernel builds", made)
        if not made:
            return

        q = Qemu(KERNEL, INITRD, disk=disk, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=60)
            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=30)
            time.sleep(4)
            q.mouse_home()

            # The same window as on the other kernel, so the viewer
            # lands in the same slot of the cascade and the reading
            # grid below is the same one
            q.mouse_goto(24, 72)               # the "Clock" icon
            q.mouse_click()
            time.sleep(4)
            q.mouse_goto(600, 430)             # the keys back to the shell
            q.mouse_click()
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
            said = status(at, vx=vx, vy=vy)
            check("(64) the viewer took the JPEG apart into pixels ('%s')" % said,
                  "64x48" in said and "loaded" in said)

            cx, cy = vx + 1, vy + 11
            reds = at(cx + 16, cy + 12)
            greens = at(cx + 48, cy + 12)
            blues = at(cx + 16, cy + 36)
            whites = at(cx + 48, cy + 36)
            check("(64) and four colours came out of it, the same as on "
                  "32 bitach (%s %s %s %s)"
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
