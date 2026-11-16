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

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary

from usbredir import Redir   # noqa: E402
from usbcam import UvcCamera, MODES  # noqa: E402


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


def main():
    machine(bulk=False)
    machine(bulk=True)
    return summary()


sys.exit(main())
