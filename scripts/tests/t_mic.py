#!/usr/bin/env python3
"""ThreadOS smoke test: the microphone, and the dot that says it listens.

QEMU's HDA codec with a microphone (hda-micro) takes its sound from
SDL here, and SDL's disk driver hands a capture the samples of a file,
a buffer at a time and no faster than they would come in: so the test
knows exactly what the microphone hears, and when. (ALSA's file plugin
would hand them over too, but as fast as they are asked for: seconds of
sound in a moment.) The samples are a pseudo-random sequence, every
stretch of which occurs once, and the mixing engine is off on both
sides, so nothing on the way changes a sample. The driver (kernel/hda.c) must:

  find the codec's microphone pin and the way from it to an input
  converter ('mic' names them)

  record: 'mic record 4 <file>' writes a WAV of four seconds, 48 kHz,
  16 bits, two channels - and after the silence before the stream
  starts, its samples are one unbroken stretch of the file's, sample
  for sample

  and the Recorder app: its button records - the bar has an orange dot
  while it does, and only then - and again stops, and the recording is
  kept in the Recordings folder of the home, again an unbroken stretch
  of what went in (after, at most, a buffer QEMU kept from the capture
  before); killed while recording, the microphone stops all the same
  and the dot goes

(The dot is looked for while a program records, not while the shell
does: a shell command holds up the desktop until it is done.)

Run on its own or through scripts/run_tests.py.
"""

import os
import random
import re
import shutil
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import ptest                 # noqa: E402
from ptest import *          # noqa: F401,F403,E402
from ptest import check, summary, _kernel_define  # noqa: E402

_R = "user64/grecord.c"
REC_W0 = _kernel_define(_R, "W0")
REC_H0 = _kernel_define(_R, "H0")
BUTTON_D = _kernel_define(_R, "BUTTON_D")
BUTTON_Y = _kernel_define(_R, "BUTTON_Y")

DOT_RGB = (0xFF, 0x9F, 0x0A)        # MIC_DOT_RGB, kernel/include/look.h
SECONDS_IN = 40
STALE = 2048                        # frames: a buffer of QEMU's, and then some


def signal():
    """Forty seconds of stereo frames, pseudo-random: no stretch of a
    few frames comes twice"""
    return random.Random(12345).randbytes(SECONDS_IN * 48000 * 4)


def main():
    work = tempfile.mkdtemp()
    raw = signal()
    infile = os.path.join(work, "in.raw")
    with open(infile, "wb") as fh:
        fh.write(raw)
    sdl = {"SDL_AUDIODRIVER": "disk", "SDL_DISKAUDIOFILEIN": infile,
           "SDL_DISKAUDIOFILE": os.devnull}
    os.environ.update(sdl)
    disk = os.path.join(work, "disk.img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    port, stop_http = start_http_server()
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=os.path.join(work, "s.log"),
                 devices=["-audiodev",
                          "sdl,id=mic0,in.mixing-engine=off,"
                          "out.mixing-engine=off",
                          "-device", "intel-hda",
                          "-device", "hda-micro,audiodev=mic0"])
        try:
            check("the desktop came up", q.wait_desktop())
            W, H = q.size()
            q.mouse_home()

            def dot():
                _, _, at = q.screen_raw()
                return any(at(x, BAR_H // 2) == DOT_RGB
                           for x in range(W // 3, W))

            q.click_desktop()
            out = q.command("mic", "mic: ", timeout=10)
            line = next((l for l in out.splitlines()
                         if l.startswith("mic: ")), "")
            check("'mic' names the microphone and its way in ('%s')"
                  % line.strip(), "mic: microphone, socket" in line and
                  "-> input" in line)
            check("no orange dot before", not dot())

            q.command("mic record 4 /home/admin/rec.wav", "mic: recorded",
                      timeout=30)
            time.sleep(1)
            check("no dot once it is done", not dot())

            def fetch(path):
                ptest.POSTED.clear()
                q.command("run curl -s --data-binary @%s "
                          "http://10.0.2.2:%d/up" % (path, port),
                          "has ended", timeout=30)
                return next((b for p_, b in ptest.POSTED if p_ == "/up"),
                            b"")

            def stretch(b):
                """Where b is in the input, as one piece, or -1"""
                at = raw.find(b[:256]) if len(b) >= 256 else -1
                if at < 0 or at % 4 or raw[at:at + len(b)] != b:
                    return -1
                return at

            def heard_of(body):
                """(frames, the silence before, the stretch of the input
                it is, or -1)"""
                ok = (len(body) > 44 and body[:4] == b"RIFF" and
                      body[8:16] == b"WAVEfmt " and
                      struct.unpack_from("<HHIIHH", body, 20) ==
                      (1, 2, 48000, 192000, 4, 16))
                if not ok:
                    return 0, 0, -1, b""
                data = body[44:]
                start = 0
                while start + 4 <= len(data) and \
                        data[start:start + 4] == b"\0\0\0\0":
                    start += 4
                heard = data[start:]
                at = stretch(heard)
                if at < 0:
                    # QEMU's SDL voice keeps the buffer it had when a
                    # capture stopped and may hand it over first when the
                    # next one starts: a piece of earlier input, no
                    # longer than STALE, before the stretch
                    old = raw.find(heard[:256]) if len(heard) >= 256 else -1
                    n = 0
                    if old >= 0 and old % 4 == 0:
                        while n < len(heard) and old + n < len(raw) and \
                                heard[n] == raw[old + n]:
                            n += 1
                        n -= n % 4
                    if 0 < n <= STALE * 4:
                        at = stretch(heard[n:])
                        if at <= old:
                            at = -1
                        heard = heard[n:]
                return len(data) // 4, start // 4, at, heard

            frames, quiet, at, heard = heard_of(fetch("/home/admin/rec.wav"))
            check("a WAV: PCM, two channels, 48 kHz, 16 bits, %d frames - "
                  "four seconds" % frames, frames == 4 * 48000)
            check("after %.2f s of silence, %.2f s of it are the very "
                  "samples that went in, in one stretch (found at %d)"
                  % (quiet / 48000, len(heard) / 4 / 48000, at),
                  at >= 0 and len(heard) >= 3 * 48000 * 4)

            # The Recorder
            q.launch_menu("grecord")
            q.mouse_goto(W - 10, H // 2)
            wx = wy = None
            end = time.time() + 15
            while time.time() < end and wx is None:
                time.sleep(0.5)
                for slot in range(8):
                    x, y = cascade(slot, REC_W0 + 2, REC_H0 + TITLE_H + 2,
                                   w=W, h=H)
                    if q.window_title(x, y, REC_W0 + 2) == "Recorder":
                        wx, wy = x, y
                        break
            check("the Recorder opens from the menu", wx is not None)
            if wx is None:
                return summary()
            bx = wx + 1 + REC_W0 // 2
            by = wy + CONTENT_DY + BUTTON_Y + BUTTON_D // 2

            def wait_dot(want, timeout=10):
                end_ = time.time() + timeout
                got = dot()
                while got != want and time.time() < end_:
                    time.sleep(0.2)
                    got = dot()
                return got

            q.click(bx, by)
            q.mouse_goto(W - 10, H // 2)
            check("its button records, and the orange dot is on the bar",
                  wait_dot(True))
            time.sleep(2.5)
            q.click(bx, by)
            q.mouse_goto(W - 10, H // 2)
            check("pressed again it stops, and the dot goes",
                  not wait_dot(False))
            said = ""
            end = time.time() + 60
            while time.time() < end and not said.startswith("Saved"):
                time.sleep(0.5)
                said = q.read_ui(wx + 1 + 12, wy + CONTENT_DY + REC_H0 - 18,
                                 300, px=11, fg=theme("dark")["app_dim"])
            check("and says it kept it ('%s')" % said,
                  said == "Saved Recording-1.wav")
            q.click_desktop()
            frames, quiet, at, heard = heard_of(
                fetch("/home/admin/Recordings/Recording-1.wav"))
            check("kept as Recordings/Recording-1.wav: %.2f s, the very "
                  "samples that went in, in one stretch (found at %d)"
                  % (frames / 48000, at),
                  at >= 0 and 2 * 48000 <= frames <= 5 * 48000 and
                  len(heard) >= 2 * 48000 * 4)

            q.click(bx, by)
            q.mouse_goto(W - 10, H // 2)
            again = wait_dot(True)
            q.click_desktop()
            q.command("ps", "NAME", timeout=10)
            time.sleep(0.5)
            out = q.serial()[q.serial().rfind("NAME"):]
            pid = next((m.group(1) for m in re.finditer(
                r"^\s*(\d+)\s+\S+\s+grecord", out, re.M)), None)
            if pid:
                q.command("kill %s" % pid, "kill:", timeout=10)
            q.mouse_goto(W - 10, H // 2)
            check("recording again (%s) and killed (pid %s): the microphone "
                  "stops all the same, the dot goes" % (again, pid),
                  again and pid and not wait_dot(False, 15))
        finally:
            q.close()
    finally:
        stop_http()
        for k in sdl:
            os.environ.pop(k, None)
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
