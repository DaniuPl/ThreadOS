#!/usr/bin/env python3
"""ThreadOS smoke test: the camera, stage one - found, described, asked.

QEMU has no webcam, so scripts/usbcam.py makes one up and plugs it into
the xHCI controller through usb-redir, at high speed: UVC 1.10, MJPEG
and YUY2 in four sizes each, eight alternate settings with isochronous
endpoints of 192 to 3072 bytes a microframe (four of them with extra
transactions in bits 12:11) - a configuration longer than 512 bytes,
as a real camera's is, which has to be read whole. The driver
(kernel/uvc.c) must:

  find it at start and say so ('[ OK ] Camera: 0c45:6366, UVC 1.10,
  8 modes')

  list every mode in 'camera' exactly as the camera's own descriptors
  and PROBE answers make them - the payload each mode needs and the
  smallest alternate that carries it, or that none does - worked out
  independently by usbcam.py from the same bytes

  have asked with one PROBE (SET_CUR, then GET_CUR) per mode, 34 bytes
  as UVC 1.10 has it, with that mode's format, frame and interval, and
  done nothing more: no COMMIT, no alternate but 0 - the camera stays
  dark until a picture is wanted

  show it in 'devices' as working under uvc

  take pictures: 'camera grab' in an uncompressed mode (YUY2 176x144)
  and in MJPEG (160x120) - COMMIT, the endpoint, the alternate, the
  isochronous stream through QEMU's xHCI and usb-redir - and every
  frame it hands out must be whole: its CRC-32 one of the frames the
  camera sent, an uncompressed one exactly w x h x 2 bytes; then back
  to alternate 0 and the stream stopped

and a second camera that streams over a bulk endpoint, with no
alternates, is listed with 'bulk' for every mode (and says it streams
in a later stage).

Then the Camera app on the desktop, with a camera whose MJPEG frames
leave their Huffman tables out, as a webcam's do:

  while it is open the bar has a green dot, and only then

  its preview of the MJPEG mode shows the four quarters of the picture
  as Pillow decodes the JPEG - which needed the standard tables

  the shutter keeps a photo in the home's Gallery folder: the frame the
  camera sent with the standard tables put back, which Pillow opens to
  the very same pixels as the original

  the mode button goes to YUY2 176x144, shown at its own size in the
  BT.601 colours of its bars, and a photo of it is a PNG of exactly
  those colours

  closing the window turns the camera off - the dot goes, the stream
  stops - and so does killing the program

Run on its own or through scripts/run_tests.py.
"""

import io
import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import ptest                 # noqa: E402
from ptest import *          # noqa: F401,F403
from ptest import check, summary, _kernel_define

_C = "user64/gcamera.c"
CAM_W0 = _kernel_define(_C, "W0")
CAM_H0 = _kernel_define(_C, "H0")
STRIP_H = _kernel_define(_C, "STRIP_H")
SHUTTER_D = _kernel_define(_C, "SHUTTER_D")
MODE_W = _kernel_define(_C, "MODE_W")
DOT_RGB = (0x30, 0xD1, 0x58)            # CAM_DOT_RGB, kernel/include/look.h

from usbredir import Redir   # noqa: E402
from usbcam import (UvcCamera, MODES, BARS, quarters_jpeg,  # noqa: E402
                    strip_dht, yuy2_rgb)


def machine(bulk):
    kind = "a bulk camera" if bulk else "an isochronous camera"
    print("--- %s on the xHCI bus ---" % kind)
    cam = UvcCamera(bulk=bulk)
    r = Redir(cam)
    work = tempfile.mkdtemp()
    try:
        q = Qemu(KERNEL, INITRD, append=TEXT_MODE,
                 serial=os.path.join(work, "s.log"),
                 devices=["-device", "qemu-xhci,id=xhci", *r.qemu_args(0)])
        try:
            q.wait_serial("thread64>", timeout=90)
            try:
                q.wait_serial("[ OK ] Camera:", timeout=60)
            except Exception:
                pass
            line = next((l for l in q.serial().splitlines()
                         if "[ OK ] Camera:" in l), "")
            if not bulk:
                # (a bulk camera has no alternates, and is shorter)
                check("its configuration is longer than 512 bytes (%d)"
                      % len(cam.config), len(cam.config) > 512)
            check("found at start: '%s'" % line.strip(),
                  "Camera: 0c45:6366, UVC 1.10, %d modes" % len(MODES)
                  in line)

            out = q.command("camera", "modes", timeout=20)
            # (the mode lines follow the header; wait for the last one)
            q.wait_serial("  mode %d:" % (len(MODES) - 1), timeout=10)
            text = q.serial()[q.serial().rfind("camera: "):]
            head = text.splitlines()[0] if text else ""
            check("'camera' names it and its port ('%s')" % head,
                  re.match(r"camera: 0c45:6366, UVC 1\.10, port \d+, "
                           r"%d modes$" % len(MODES), head.strip()) is not None)
            got = [l.rstrip() for l in text.splitlines()
                   if l.startswith("  mode ")]
            want = cam.expected()
            for g, w in zip(got + [""] * len(want), want):
                check("as the camera's own bytes make it: '%s'" % w.strip(),
                      g == w)

            probes = cam.probes()
            asked = [(f, fr, iv) for f, fr, iv, _ in probes]
            check("one PROBE per mode, with its format, frame and interval "
                  "(%s)" % asked,
                  asked == [(f, fr, iv) for f, fr, _, _, iv, _ in MODES])
            check("each 34 bytes long, as UVC 1.10 has it (%s)"
                  % [n for _, _, _, n in probes],
                  probes and all(n == 34 for _, _, _, n in probes))
            reads = [r_ for r_ in cam.requests if r_[0] == "get_probe"]
            check("each read back (%d GET_CUR)" % len(reads),
                  len(reads) == len(MODES))
            commits = [r_ for r_ in cam.requests
                       if r_[0] in ("set_commit", "get_commit")]
            alts = [r_ for r_ in cam.requests if r_[0] == "set_alt"]
            check("and nothing more: no COMMIT (%d), only alternate 0 (%s)"
                  % (len(commits), alts),
                  not commits and alts and all(a == ("set_alt", 1, 0)
                                               for a in alts))

            out = q.command("devices", "usb socket", timeout=20)
            dev = next((l for l in q.serial().splitlines()
                        if "0c45:6366" in l and "usb socket" in l), "")
            check("'devices' shows it working under uvc ('%s')" % dev.strip(),
                  dev.rstrip().endswith("working (uvc)"))

            if bulk:
                out = q.command("camera grab 0 1", "camera: ", timeout=20)
                check("a bulk camera does not stream yet, and says so",
                      "a bulk camera" in out)
            else:
                for mode in (6, 3):
                    grab(q, cam, mode, 3)
        finally:
            q.close()
    finally:
        r.close()


def grab(q, cam, mode, n):
    f, fr, w, h, _, _ = MODES[mode]
    kind = "YUY2" if f == 2 else "MJPG"
    before = len(cam.requests)
    out = q.command("camera grab %d %d" % (mode, n), "frames, ", timeout=60)
    frames = re.findall(r"frame (\d+): (\d+)x(\d+) (\w{4}), (\d+) bytes, "
                        r"crc32 ([0-9a-f]+)", out)
    summary_line = next((l for l in out.splitlines()
                         if l.startswith("camera: ") and "frames" in l), "")
    check("%s %dx%d: %d frames come (%s)" % (kind, w, h, n,
                                            summary_line.strip()),
          len(frames) == n and ("%d of %d frames" % (n, n)) in summary_line)
    sent = set(cam.sent_crcs)
    whole = [int(c, 16) in sent for *_, c in frames]
    check("every one whole: its CRC-32 one the camera sent (%s)" % whole,
          frames and all(whole))
    if f == 2:
        check("an uncompressed one exactly %d bytes (%s)" % (w * h * 2,
              [int(b) for *_, b, _ in frames]),
              frames and all(int(b) == w * h * 2 for *_, b, _ in frames))
    else:
        check("each a JPEG the camera sent, %s bytes"
              % [int(b) for *_, b, _ in frames],
              frames and all(int(b) > 100 for *_, b, _ in frames))
    later = cam.requests[before:]
    commits = [r for r in later if r[0] == "set_commit"]
    alts = [r for r in later if r[0] == "set_alt"]
    want_alt = next(int(m.group(1)) for m in [re.search(
        r"alternate (\d+)", cam.expected()[mode])])
    check("committed (%s), alternate %d and then 0 (%s), the stream "
          "started and stopped"
          % ([(c[2][2], c[2][3]) for c in commits], want_alt, alts),
          len(commits) == 1 and commits[0][2][2:4] == bytes([f, fr]) and
          [a[2] for a in alts] == [want_alt, 0] and
          ("start_iso", 0x81) in later and ("stop_iso", 0x81) in later)


def app_machine():
    print("--- the Camera app: the dot, the preview, photos ---")
    from PIL import Image
    cam = UvcCamera(still=True, strip_dht=True)
    r = Redir(cam)
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    port, stop_http = start_http_server()
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog,
                 devices=["-device", "qemu-xhci,id=xhci", *r.qemu_args(0)])
        try:
            check("the desktop came up", q.wait_desktop())
            W, H = q.size()
            q.mouse_home()

            def dot():
                _, _, at = q.screen_raw()
                return any(at(x, BAR_H // 2) == DOT_RGB
                           for x in range(W // 3, W))

            def wait_dot(want, timeout=10):
                end = time.time() + timeout
                got = dot()
                while got != want and time.time() < end:
                    time.sleep(0.3)
                    got = dot()
                return got

            check("no green dot on the bar before", not dot())
            q.launch_menu("gcamera")
            q.mouse_goto(W - 10, H // 2)
            wx = wy = None
            end = time.time() + 15
            while time.time() < end and wx is None:
                time.sleep(0.5)
                for slot in range(8):
                    x, y = cascade(slot, CAM_W0 + 2, CAM_H0 + TITLE_H + 2,
                                   w=W, h=H)
                    if q.window_title(x, y, CAM_W0 + 2) == "Camera":
                        wx, wy = x, y
                        break
            check("the Camera opens from the menu", wx is not None)
            if wx is None:
                return
            cx, cy = wx + 1, wy + CONTENT_DY
            check("and the green dot is on the bar while it is open",
                  wait_dot(True))

            # MJPEG 640x480, shown at half size in the middle
            ph = CAM_H0 - STRIP_H
            ox, oy = cx + (CAM_W0 - 320) // 2, cy + (ph - 240) // 2
            ref = Image.open(io.BytesIO(quarters_jpeg(640, 480))) \
                .convert("RGB")
            pts = [(80 + (i % 2) * 160, 60 + (i // 2) * 120)
                   for i in range(4)]
            want = [ref.getpixel((2 * x, 2 * y)) for x, y in pts]
            got = []
            end = time.time() + 15
            while time.time() < end:
                _, _, at = q.screen_raw()
                got = [at(ox + x, oy + y) for x, y in pts]
                if all(max(abs(a - b) for a, b in zip(g, w)) <= 6
                       for g, w in zip(got, want)):
                    break
                time.sleep(0.5)
            check("the MJPEG preview, which has no Huffman tables of its "
                  "own: the quarters as Pillow decodes them (%s / %s)"
                  % (got, want),
                  all(max(abs(a - b) for a, b in zip(g, w)) <= 6
                      for g, w in zip(got, want)))

            def shutter_and_fetch(name):
                q.mouse_goto(cx + CAM_W0 // 2, cy + 40)
                q.sendkey("spc")
                time.sleep(1)
                q.mouse_goto(W - 10, H // 2)
                q.click_desktop()
                listing = ""
                end = time.time() + 15
                while time.time() < end and name not in listing:
                    q.command("cd /home/admin/Gallery")
                    listing = q.command("fls", "Used:", timeout=10)
                    time.sleep(0.5)
                ptest.POSTED.clear()
                q.command("run curl -s --data-binary @/home/admin/Gallery/%s "
                          "http://10.0.2.2:%d/up" % (name, port),
                          "has ended", timeout=30)
                body = next((b for p_, b in ptest.POSTED if p_ == "/up"),
                            None)
                return name in listing, body

            listed, body = shutter_and_fetch("Photo-1.jpg")
            frame = cam.jpegs.get((1, 640, 480))
            ok = False
            if body and frame:
                try:
                    saved = Image.open(io.BytesIO(body)).convert("RGB")
                    ok = saved.tobytes() == ref.tobytes()
                except Exception:
                    ok = False
            check("the shutter keeps Photo-1.jpg in the Gallery folder (%s)"
                  % listed, listed)
            check("the frame itself with the standard tables put back: "
                  "without them it is the camera's bytes (%s), and Pillow "
                  "opens it to the original's pixels (%s)"
                  % (body is not None and strip_dht(body) == frame, ok),
                  body is not None and strip_dht(body) == frame and
                  b"\xff\xc4" in body and ok)

            # The mode button, four times: YUY2 176x144
            q.mouse_goto(cx + CAM_W0 // 2, cy + 40)
            for _ in range(4):
                q.click(cx + 8 + MODE_W // 2, cy + CAM_H0 - STRIP_H // 2)
                time.sleep(1.5)
            ox, oy = cx + (CAM_W0 - 176) // 2, cy + (ph - 144) // 2
            want = [yuy2_rgb(*b) for b in BARS]
            xs = [k * 176 // 8 + 11 for k in range(8)]
            got = []
            end = time.time() + 15
            while time.time() < end:
                _, _, at = q.screen_raw()
                got = [at(ox + x, oy + 72) for x in xs]
                if got == want:
                    break
                time.sleep(0.5)
            check("the mode button: YUY2 176x144 at its own size, its bars "
                  "in their BT.601 colours (%s)" % (got == want,), got == want)
            listed, body = shutter_and_fetch("Photo-2.png")
            exact = False
            if body:
                try:
                    pic = Image.open(io.BytesIO(body)).convert("RGB")
                    exact = pic.size == (176, 144) and all(
                        pic.getpixel((x, y)) ==
                        yuy2_rgb(*BARS[(x - x % 2) * 8 // 176])
                        for y in (0, 71, 143) for x in range(176))
                except Exception:
                    exact = False
            check("its photo, Photo-2.png, those very colours, every pixel "
                  "(%s, %s)" % (listed, exact), listed and exact)

            # Closing turns it off
            q.click(*light_xy(wx, wy, 0))
            q.mouse_goto(W - 10, H // 2)
            gone = not wait_dot(False)
            check("closed, the camera is off: no dot (%s), the stream stopped"
                  ", alternate 0" % gone, gone and
                  cam.requests and ("stop_iso", 0x81) in cam.requests and
                  [x for x in cam.requests if x[0] == "set_alt"][-1] ==
                  ("set_alt", 1, 0))

            # ... and so does a program that is killed
            q.launch_menu("gcamera")
            q.mouse_goto(W - 10, H // 2)
            check("opened again, the dot again", wait_dot(True, 20))
            q.click_desktop()
            out = q.command("ps", "NAME", timeout=10)
            time.sleep(0.5)
            out = q.serial()[q.serial().rfind("NAME"):]
            pid = next((m.group(1) for m in re.finditer(
                r"^\s*(\d+)\s+\S+\s+gcamera", out, re.M)), None)
            stops = cam.requests.count(("stop_iso", 0x81))
            if pid:
                q.command("kill %s" % pid, "kill:", timeout=10)
            q.mouse_goto(W - 10, H // 2)
            gone = not wait_dot(False, 15)
            check("killed (pid %s), the camera is off all the same: no dot "
                  "(%s), the stream stopped" % (pid, gone),
                  pid and gone and
                  cam.requests.count(("stop_iso", 0x81)) > stops)
        finally:
            q.close()
    finally:
        stop_http()
        r.close()
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)


def main():
    machine(bulk=False)
    machine(bulk=True)
    app_machine()
    return summary()


sys.exit(main())
