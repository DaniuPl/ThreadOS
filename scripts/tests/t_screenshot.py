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


def view(q, name):
    """The viewer started from the shell on a file, and what its status
    line says once it has said something about the file"""
    q.click_desktop()                   # the keys to the shell
    q.type_line("run gimg " + name)
    said = ""
    end = time.time() + 20
    while time.time() < end:
        time.sleep(1)
        q.mouse_goto(560, 430)
        said = status(q)
        if "loaded" in said or "not" in said:
            break
    return said


def close_viewer(q):
    """The shell waits for the program it ran, so the viewer is closed
    before the shell is asked for anything again"""
    seen = q.serial().count("[gimg] done")
    q.click(*light_xy(VX, VY, 0))
    end = time.time() + 15
    while time.time() < end and q.serial().count("[gimg] done") == seen:
        time.sleep(0.3)


def fetch(q, port, path, name):
    """httpget from the test's own server, and what it said"""
    q.click_desktop()
    return q.command("run httpget 10.0.2.2 %d /%s %s" % (port, path, name),
                     "[httpget] done", timeout=30)


def quarters(at):
    """The middle of each quarter of the 64x48 picture in the viewer"""
    return tuple(at(CX + x, CY + y)
                 for x, y in ((16, 12), (48, 12), (16, 36), (48, 36)))


def saturated(at):
    """Red, green, blue and white, each the colour it is and nothing
    in between"""
    reds, greens, blues, whites = quarters(at)
    return (reds[0] > 200 and reds[1] < 60 and
            greens[1] > 200 and greens[0] < 60 and
            blues[2] > 200 and blues[0] < 60 and
            all(v > 200 for v in whites))


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")

    http_port, stop_http = start_http_server()
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop())
            q.mouse_home()

            # Something on the screen worth photographing
            q.launch_menu("gclock")                # the "Clock" tile
            time.sleep(4)
            q.mouse_goto(420, 300)
            time.sleep(1.5)

            # What the picture will have to repeat: the dock, which does
            # not change while nothing is clicked - unlike the clocks.
            # The viewer shows a screen that does not fit its window at
            # every step-th pixel each way (gimg.c: the least step that
            # fits), so the places remembered are on that grid.
            sw, sh = q.size()
            room_h = VH - GIMG_STATUS_H - 2
            step = 1
            while sw // step > VW or sh // step > room_h:
                step += 1
            w, h, at = q.screen_raw()
            g = dock_geom(n_w=1, w=sw, h=sh)
            spots = [(x, y) for y in range(g["y"] + 2, g["y"] + g["h"] - 2, 4)
                     for x in range(g["x"] + 2, g["x"] + g["w"] - 2, 6)]
            spots = sorted({(x - x % step, y - y % step) for x, y in spots})
            before = {p: at(*p) for p in spots}
            check("the dock is on the screen (%d places in it remembered)"
                  % len(spots), len(set(before.values())) > 50)

            # --- Print Screen -------------------------------------------
            q.sendkey("print")
            q.click_desktop()
            listing = ""
            m = None
            end = time.time() + 20          # writing to the disk takes a while
            while time.time() < end and not m:
                time.sleep(1)
                listing = q.command("fls", "Used:", timeout=10)
                m = re.search(r"shot1\.psc\s+\((\d+) B\)", listing)
            check("Print Screen wrote a file to the disk ('%s')"
                  % (m.group(0) if m else listing[-60:].replace("\n", " ")),
                  m is not None)
            if not m:
                return summary()
            # Three bytes a pixel of the screen, after "TSC2" and two sizes
            check("and it has exactly as many bytes as the screen in colours "
                  "(%s for %dx%d)" % (m.group(1), sw, sh),
                  int(m.group(1)) == sw * sh * 3 + 4 + 2 + 2)

            # --- and the viewer puts it back on the screen --------------
            said = view(q, "shot1.psc")
            check("the viewer says what it loaded ('%s')" % said,
                  "%dx%d" % (sw, sh) in said and "loaded" in said)
            check("and that it shows every %d. pixel, because the whole "
                  "screen does not fit in a window on this screen" % step,
                  step > 1 and "step %d" % step in said)

            # The picture IS the screen: the dock, inside the window, at
            # a step-th of the size and in exactly the colours it had.
            # The viewer draws the picture a row at a time - hundreds of
            # rows through as many calls - so the screen is looked at
            # again until it stops filling up.
            #
            # Sixty readings rather than twenty-five. Hundreds of system
            # calls take as long as the machine gives them, and the
            # machine is four QEMUs sharing a host that is also running
            # the rest of the suite. Waiting longer for the same answer
            # is patience, lowering the answer would be something else.
            same = 0
            for _ in range(60):
                w, h, at = q.screen_raw()
                same = sum(1 for (x, y), c in before.items()
                           if at(CX + x // step, CY + y // step) == c)
                if same == len(before):
                    break
                time.sleep(1)
            check("the picture in the window really is the screen - the dock "
                  "is there, every remembered pixel in its colour (%d of %d)"
                  % (same, len(before)), same == len(before))
            close_viewer(q)

            # --- the counter, and what is not a picture -----------------
            q.click_desktop()
            time.sleep(1)
            q.sendkey("print")
            listing = ""
            end = time.time() + 20
            while time.time() < end and "shot2.psc" not in listing:
                time.sleep(1)
                listing = q.command("fls", "Used:", timeout=10)
            check("the second shot gets the next number",
                  "shot2.psc" in listing and "shot1.psc" in listing)

            # (a text file of the account's own, long enough for the
            # viewer to look at its first bytes and judge)
            q.command("write words.txt these are words and nothing but "
                      "words, a picture of nothing", "written", timeout=20)
            said = view(q, "words.txt")
            check("a file that is not a picture is refused with "
                  "a word of explanation ('%s')" % said,
                  "is not a picture" in said)
            close_viewer(q)

            # --- a picture from the outside world -----------------------
            # No floppy, no shared folder - but a network. The machine
            # fetches the file itself and then shows it - in the place
            # the closed viewers stood in, the next free slot of the
            # cascade.
            got = fetch(q, http_port, "squares.bmp", "picture.bmp")
            check("the machine fetches a picture off the network onto the disk",
                  "HTTP 200" in got and "has ended (code 0)" in got)

            said = view(q, "picture.bmp")
            w, h, at = q.screen_raw()
            check("the viewer loaded the BMP ('%s')" % said,
                  "64x48" in said and "loaded" in said)

            # The four quarters, in the four colours the file has -
            # each of them exactly in the palette, so 'nearly' is not
            # good enough here
            check("and four quarters in four colours came out of it "
                  "(%s)" % (quarters(at),), saturated(at))
            close_viewer(q)

            # --- and the same picture as a PNG --------------------------
            # Chunks, a deflate stream and a filter byte per row: three
            # things that all have to be right, and the four exact
            # colours say whether they were.
            got = fetch(q, http_port, "squares.png", "picture.png")
            check("the machine fetches a PNG off the network",
                  "HTTP 200" in got and "has ended (code 0)" in got)

            said = view(q, "picture.png")
            w, h, at = q.screen_raw()
            check("the viewer unpacked the PNG ('%s')" % said,
                  "64x48" in said and "loaded" in said)
            check("and the same four colours came out of it (%s)"
                  % (quarters(at),), saturated(at))
            close_viewer(q)

            # --- and as a JPEG, which is what a camera writes ------------
            # A different kind of work again: Huffman codes, a cosine
            # transform undone in fixed point, and colour stored at half
            # resolution and put back. The file comes from an outside
            # encoder, so this is our arithmetic reading somebody
            # else's picture.
            got = fetch(q, http_port, "squares.jpg", "picture.jpg")
            check("the machine fetches a JPEG off the network",
                  "HTTP 200" in got and "has ended (code 0)" in got)

            said = view(q, "picture.jpg")
            w, h, at = q.screen_raw()
            check("the viewer took the JPEG apart into pixels ('%s')" % said,
                  "64x48" in said and "loaded" in said)

            # JPEG is lossy, so the colours are read well inside the
            # quarters - but they still have to be the right four, and
            # a decoder that got the transform wrong gives grey mush
            # rather than four saturated fields.
            check("and the same four colours came out of it (%s)"
                  % (quarters(at),), saturated(at))
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
            q.wait_desktop()
            time.sleep(4)
            q.mouse_home()

            # The same window as on the other kernel, so the viewer
            # lands in the same slot of the cascade and the reading
            # grid below is the same one
            q.launch_menu("gclock")                # the "Clock" tile
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
