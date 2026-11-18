#!/usr/bin/env python3
"""ThreadOS smoke test: the phone's speaker and microphone.

QEMU's virtio-sound-device on the ARM64 kernel (kernelarm64/vsnd.c),
with SDL's disk driver behind it: what the phone plays is written to a
file, at the pace it would sound, and what its microphone hears is read
from one - a pseudo-random sequence of which every stretch is unique,
so where a recording comes from is found by looking for it. The mixing
engine is off both ways, so nothing on the way changes a sample.

A program, libc/tests/sndtest.c, through the PC's calls:

  the kernel finds a speaker and a microphone; SYS64_SND_GET says so,
  and SYS64_MIC names the microphone, 48 kHz and two channels

  three plays, each a sequence the test computes: at full loudness the
  very samples, one unbroken stretch; at 50 every sample halved as the
  kernel scales it (towards zero); muted, nothing of the third - the
  file stays silent between the second and the fourth for at least as
  long as the third lasted. And none cut short: the end of a play waits
  until the device has sounded what it took

  two seconds recorded: after the silence before the first samples,
  what the program got is a stretch of what the microphone was given,
  byte for byte (CRC-32 over it all)

  while one program plays, another is turned away; a program that ends
  while it plays lets the speaker go

and the phone's shell (kernelarm64/mobsound.c):

  Settings name the speaker; a program's notification sounds the
  chime - two notes, of the frequencies and lengths mobsound.c gives,
  fading out, the same on both channels - and with silent mode on, the
  next makes no sound

  the Recorder: its button records - an orange dot on the status bar
  while it does, its time counting up - and again stops; the recording
  is listed with its length, and a tap on it plays it: back comes what
  the microphone heard, sample for sample
"""

import os
import random
import re
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, noarm64, arm64_boot, Phone, ARM64_INITRD,
                   _kernel_define)
from t_arm64apps import (App, m, a, row_y, page, dock_xy, K, LW, LH, MUTED,
                         WHITE, SWITCH_X)
from t_arm64mobile import apps as home_apps

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
SND = "kernelarm64/mobsound.c"
RATE = 48000
DOT = (0xFF, 0x9F, 0x0A)                 # MIC_DOT_RGB, kernel/include/look.h
AUDIO = ["-audiodev", "sdl,id=snd0,in.mixing-engine=off,"
         "out.mixing-engine=off",
         "-device", "virtio-sound-device,audiodev=snd0"]


def s(name):
    return _kernel_define(SND, name)


def sequence(frames, seed, vol=100):
    """sndtest's xorshift32 samples, scaled as vsnd.c scales them"""
    r, out = seed, []
    for _ in range(frames * 2):
        r ^= (r << 13) & 0xFFFFFFFF
        r ^= r >> 17
        r ^= (r << 5) & 0xFFFFFFFF
        v = r >> 16
        v = v - 65536 if v >= 32768 else v
        if vol != 100:
            q = abs(v * vol) // 100
            v = q if v >= 0 else -q
        out.append(v)
    return struct.pack("<%dh" % len(out), *out)


def stretch(hay, b):
    """Where all of b is in hay, frame-aligned, or -1"""
    at = hay.find(b[:256])
    while at >= 0:
        if at % 4 == 0 and hay[at:at + len(b)] == b:
            return at
        at = hay.find(b[:256], at + 1)
    return -1


def run_of(hay, at, b):
    """How many bytes from hay[at:] are b's, from its start"""
    n = 0
    while n < len(b) and at + n < len(hay) and hay[at + n] == b[n]:
        n += 1
    return n - n % 4


def sound_env(work, seconds):
    raw = random.Random(4321).randbytes(seconds * RATE * 4)
    infile, outfile = os.path.join(work, "in.raw"), os.path.join(work,
                                                                   "out.raw")
    with open(infile, "wb") as fh:
        fh.write(raw)
    os.environ.update({"SDL_AUDIODRIVER": "disk",
                       "SDL_DISKAUDIOFILEIN": infile,
                       "SDL_DISKAUDIOFILE": outfile})
    return raw, outfile


def program(work):
    print("--- a program's sound ---")
    raw, outfile = sound_env(work, 20)
    steps = ["info", "play:24000:1:100:0", "play:24000:2:50:0",
             "play:9600:3:100:1", "play:24000:4:100:0", "rec:96000", "busy"]
    said = arm64_boot(cores=2, mib=512, initrd=ARM64_INITRD, timeout=120,
                      append="run=" + ",".join(["sndtest"] + steps),
                      extra=["-global", "virtio-mmio.force-legacy=false"] +
                      AUDIO)
    check("the kernel finds the speaker and the microphone",
          "Sound: virtio-sound, 2 streams - a speaker, a microphone" in said)
    check("SND_GET: a speaker, full loudness, not muted",
          "[sndtest] speaker 1 volume 100 muted 0 steps 101" in said)
    check("SYS64_MIC: the microphone, 48 kHz, two channels",
          "[sndtest] microphone 1 'virtio-sound, input' 48000 Hz 2 channels"
          in said)
    out = open(outfile, "rb").read() if os.path.exists(outfile) else b""

    one = sequence(24000, 1)
    at1 = stretch(out, one)
    check("the first play's samples, every one, in one stretch (at %d)"
          % at1, at1 >= 0)
    two = sequence(24000, 2, 50)
    at2 = stretch(out, two)
    check("the second at 50: each sample halved towards zero, in one "
          "stretch, after the first (at %d)" % at2, at2 > at1 >= 0)
    four = sequence(24000, 4)
    at4 = stretch(out, four)
    check("the fourth, after them (at %d)" % at4, at4 > at2 >= 0)
    three = sequence(9600, 3)
    gap = out[at2 + len(two):at4] if at4 > at2 >= 0 else b""
    check("muted, the third is not there", out.find(three[:64]) < 0)
    check("silence between the second and the fourth: %d frames, at least "
          "the third's 9600" % (len(gap) // 4),
          len(gap) >= 9600 * 4 and gap.count(0) == len(gap))

    rec = re.search(r"\[sndtest\] recorded (\d+) frames, (\d+) silent first;"
                    r" heard ([0-9a-f]*) crc ([0-9a-f]{8})", said)
    total = quiet = 0
    at = -1
    if rec:
        total, quiet = int(rec.group(1)), int(rec.group(2))
        heard = (total - quiet) * 4
        at = raw.find(bytes.fromhex(rec.group(3)))
        same = (at >= 0 and at % 4 == 0 and
                zlib.crc32(raw[at:at + heard]) == int(rec.group(4), 16))
    check("two seconds recorded (%d frames)" % total, total == 96000)
    check("after %.2f s of silence, %.2f s of the very samples the "
          "microphone was given, in one stretch (from %d)"
          % (quiet / RATE, (total - quiet) / RATE, at),
          rec is not None and same and total - quiet >= 1.5 * RATE)
    check("a second player is turned away while one plays; a player "
          "ending without its end lets the speaker go",
          "[sndtest] busy: first 0, a second turned away 1, the child played "
          "1, free again 0" in said)
    check("sndtest ended with code 0",
          "[proc] sndtest (pid 1) ended with code 0" in said)
    pages = re.search(r"pages free: (\d+) before the programs, (\d+) after",
                      said)
    check("and every page it had is free again",
          pages is not None and pages.group(1) == pages.group(2))


def chime_notes():
    src = open(os.path.join(ROOT, SND)).read()
    body = re.search(r"chime\[\] = \{(.*?)\};", src, re.S).group(1)
    return [(int(h), int(ms)) for h, ms in
            re.findall(r"\{\s*(\d+),\s*(\d+)\s*\}", body)]


def frequency(x):
    """Upward zero crossings over the time between the first and last"""
    cross = [i for i in range(1, len(x)) if x[i - 1] <= 0 < x[i]]
    if len(cross) < 3:
        return 0.0
    return (len(cross) - 1) * RATE / (cross[-1] - cross[0])


def frames_of(b):
    n = len(b) // 4
    v = struct.unpack("<%dh" % (2 * n), b[:4 * n])
    return v[0::2], v[1::2]


def size(path):
    return os.path.getsize(path) if os.path.exists(path) else 0


def chime(app, outfile):
    """The first notification's chime, from where the file was before"""
    q = app.q
    out = open(outfile, "rb").read()
    left, right = frames_of(out)
    loud = [i for i, v in enumerate(left) if v]
    notes = chime_notes()
    if not loud:
        check("the notification sounds", False)
        return
    first = loud[0]
    last = max(i for i in loud if i < first + RATE)
    ms = (last - first + 1) * 1000 / RATE
    want_ms = sum(n[1] for n in notes)
    check("the notification sounds, %.1f ms of it (%d ms in mobsound.c)"
          % (ms, want_ms), abs(ms - want_ms) <= 2)
    peak = max(abs(v) for v in left[first:last + 1])
    check("fading from its peak, %d (CHIME_PEAK %d)" % (peak, s("CHIME_PEAK")),
          s("CHIME_PEAK") * 0.95 <= peak <= s("CHIME_PEAK"))
    check("the same on both channels",
          left[first:last + 1] == right[first:last + 1])
    at = first
    for hz, n_ms in notes:
        n = n_ms * RATE // 1000
        part = left[at + n // 20:at + n * 3 // 4]     # its loud part
        f = frequency(part)
        check("a note of %d Hz for %d ms: %.1f Hz" % (hz, n_ms, f),
              abs(f - hz) <= hz * 0.01)
        at += n


def settings_silent(app):
    q = app.q
    app.home()
    q.tap(*[c + m("MOB_DOCK_TILE") // 2 for c in dock_xy(2, 3)])
    app.said("[mobile] Settings (dock)")
    page(app, "sound")
    app.said("[settings] Sound")
    speaker = app.read(LW // 2, LW, row_y(0) + 6, a("SET_PX"), fg=MUTED)
    check(f"Settings name the speaker ({speaker!r})",
          speaker == "virtio-sound")
    q.tap(SWITCH_X, row_y(1))
    app.said("[settings] Silent mode on")


def recorder(app, raw, outfile):
    q = app.q
    app.home()
    names = [n for n, _ in home_apps()]
    k = names.index("Recorder")
    cols = (LW - m("MOB_GRID_X") + m("MOB_GRID_STEP") - m("MOB_TILE")) // \
        m("MOB_GRID_STEP")
    tx = m("MOB_GRID_X") + (k % cols) * m("MOB_GRID_STEP") + m("MOB_TILE") // 2
    ty = m("MOB_GRID_Y") + (k // cols) * m("MOB_ROW_STEP") + m("MOB_TILE") // 2
    q.tap(tx, ty)
    app.said("[mobile] Recorder (Recorder)")

    def dot():
        w, h, at = app.screen()
        return any(at(x, 14 * K) == DOT for x in range(LW, 2 * LW))

    check("no orange dot before", not dot())
    button = (LW // 2, LH - s("REC_BUTTON_UP"))
    q.tap(*button)
    app.said("[recorder] recording 1 begins")
    check("its button records, and the bar has the orange dot", dot())
    # the screen as it is 1.5 and 3.5 seconds in, read afterwards: a
    # large font read glyph by glyph takes longer than a second
    began = time.time()
    shots = []
    for at in (1.5, 3.5):
        time.sleep(max(0, began + at - time.time()))
        shots.append(q.screen_raw())
    shown = [q.find_ui(0, LW * K, s("REC_TIME_Y") * K, px=56 * K,
                       face="bold", fg=WHITE, charset="0123456789:",
                       screen=scr) for scr in shots]
    secs = [int(t[2:]) if re.fullmatch(r"0:\d\d", t) else -1 for t in shown]
    check(f"its time counts up from zero, by two seconds in two ({shown})",
          0 <= secs[0] <= 2 and 1 <= secs[1] - secs[0] <= 3)
    seen = len(q.serial())
    q.tap(*button)
    line = q.wait_serial("[recorder] recording 1: ", timeout=20)
    got = re.search(r"\[recorder\] recording 1: (\d+) frames",
                    q.serial()[seen:])
    frames = int(got.group(1)) if got else 0
    check("again, it stops: %.2f s recorded" % (frames / RATE),
          frames >= 2 * RATE)
    time.sleep(0.5)
    check("and the dot goes", not dot())
    mid = s("REC_LIST_Y") + (s("REC_ROW_H") - 8) // 2
    scr = app.screen()
    name = app.read(70, LW - 100, mid + 6, 16, face="bold", scr=scr)
    length = app.read(LW // 2, LW, mid + 6, 15, fg=MUTED, scr=scr)
    want = "0:%02d" % (frames // RATE)
    check(f"listed: {name!r}, {length!r} ({want})",
          name == "Recording 1" and length == want)

    before = size(outfile)
    q.tap(100, mid)
    q.wait_serial("[recorder] recording 1 played", timeout=30)
    time.sleep(0.5)
    out = open(outfile, "rb").read()[before:]
    start = 0
    while start + 4 <= len(out) and out[start:start + 4] == b"\0\0\0\0":
        start += 4
    heard = out[start:]
    at = raw.find(heard[:256]) if len(heard) >= 256 else -1
    n = run_of(raw, at, heard) if at >= 0 and at % 4 == 0 else 0
    rest = heard[n:n + RATE * 4 // 2]
    check("a tap plays it: %.3f s of the very samples the microphone was "
          "given, in one stretch, of the %.3f s recorded, then silence"
          % (n / 4 / RATE, frames / RATE),
          frames - RATE // 10 <= n // 4 <= frames and
          len(rest) > 0 and rest.count(0) == len(rest))


def phone(work):
    print("--- the phone's sound ---")
    raw, outfile = sound_env(work, 60)
    q = Phone(cores=2, mib=1024, extra=AUDIO, append=(
        "run=sndtest,vol:100,notify:Hello,sleep:30000,notify:Quiet"))
    app = App(q)
    try:
        app.said("[sound] chime played", timeout=90)
        time.sleep(1)
        chime(app, outfile)
        q.wait_serial("[mobile] lock screen", timeout=60)
        q.touch([(180, 560), (180, 420), (180, 300)])
        app.said("[mobile] home")
        settings_silent(app)
        seen = len(q.serial())
        q.wait_serial("[notify] sndtest: Quiet", timeout=60)
        quiet_from = size(outfile)
        time.sleep(1.5)
        after = open(outfile, "rb").read()[quiet_from:]
        check("silent, the next notification makes no sound (%.2f s heard, "
              "all silence)" % (len(after) / 4 / RATE),
              len(after) >= RATE * 4 and after.count(0) == len(after) and
              "[sound] chime played" not in q.serial()[seen:])
        recorder(app, raw, outfile)
    except RuntimeError:
        # what the phone said last, to see where it stood
        print("\n".join(q.serial().splitlines()[-25:]))
        raise
    finally:
        q.close()


def no_sound():
    """Why this QEMU cannot run the test: virtio-sound came in 8.2, and
    SDL's audio is a module of its own in some distributions"""
    def said(*args):
        p = subprocess.run(["qemu-system-aarch64", *args], capture_output=True,
                           text=True)
        return p.stdout + p.stderr
    if "virtio-sound-device" not in said("-device", "help"):
        return "no virtio-sound-device in this QEMU (8.2 or later has it)"
    if not re.search(r"^sdl$", said("-audiodev", "help"), re.M):
        return "no SDL audio in this QEMU"
    return None


def main():
    print("--- the phone's speaker and microphone ---")
    why = noarm64() or no_sound()
    if why:
        print("SKIPPED: " + why)
        return summary()
    work = tempfile.mkdtemp()
    for part in (program, phone):
        os.mkdir(os.path.join(work, part.__name__))
        part(os.path.join(work, part.__name__))
    return summary()


if __name__ == "__main__":
    sys.exit(main())
