#!/usr/bin/env python3
"""ThreadOS smoke test: GStreamer on the phone (the ARM64 kernel).

GStreamer and FFmpeg built for aarch64 by the PC's rules (TARGET=aarch64,
into BUILD/aarch64: `make TARGET=aarch64 BUILD=<build>/aarch64
gstreamer`), the same gsttest (libc/tests/gsttest.c) run from the
initrd on the ARM64 kernel, and held to what FFmpeg on the host makes
of the same files - the PC's t_gstreamer, on the phone:

  every element WebKit's media player asks for is built in;
  H.264 in MP4 and VP9 in WebM decode, frame by frame, to the I420
  FFmpeg's framemd5 hashes - FFmpeg's aarch64 code (NEON) where the PC
  ran its x86 code, to the same bits;
  FLAC decodes to the host's PCM, byte for byte, and Opus to as many
  samples, give or take its decoder's delay;
  the tone played through threadosaudiosink comes out of the phone's
  virtio-sound speaker - SDL's disk driver writing it to a file - as
  1000 Hz for about its two seconds.
"""

import os
import re
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, noarm64, arm64_boot, wav_tone, KERNEL
from t_arm64libs import initrd, program
from t_arm64sound import AUDIO, RATE, sound_env
from t_gstreamer import make_files, host_frames, host_pcm

GSTTEST = os.path.join(os.path.dirname(os.path.abspath(KERNEL)), "aarch64",
                       "user64", "gsttest.elf")
STEPS = ["elements", "frames", "h264.mp4", "frames", "vp9.webm", "pcm",
         "tone.mkv", "pcm", "opus.webm", "play", "tone.mkv"]


def parts(body):
    """What gsttest printed for each of its steps: "frames h264.mp4" ->
    its lines"""
    out, key = {}, None
    for line in body.split("\n"):
        m = re.match(r"gsttest: (elements|(?:frames|pcm|play) \S+)$", line)
        if m:
            key = m.group(1)
            out[key] = []
        elif key:
            out[key].append(line)
    return {k: "\n".join(v) for k, v in out.items()}


def raw_tone(path):
    """The speaker's file (16-bit stereo at RATE, no header) as wav_tone
    reads a WAV"""
    if not os.path.exists(path):
        return None
    wav = path + ".wav"
    body = open(path, "rb").read()
    with open(wav, "wb") as fh:
        fh.write(b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVEfmt " +
                 struct.pack("<IHHIIHH", 16, 1, 2, RATE, RATE * 4, 4, 16) +
                 b"data" + struct.pack("<I", len(body)) + body)
    return wav_tone(wav)


def main():
    print("--- GStreamer on the ARM64 kernel ---")
    why = noarm64()
    if why:
        print("SKIPPED: " + why)
        return summary()
    if not os.path.exists(GSTTEST):
        print("SKIPPED: no aarch64 gsttest - make TARGET=aarch64 "
              "BUILD=<build>/aarch64 gstreamer <build>/aarch64/user64/"
              "gsttest.elf")
        return summary()

    work = tempfile.mkdtemp()
    files = make_files(work)
    check(f"the host's FFmpeg makes the test's media ({sorted(files)})",
          len(files) == 4)
    if len(files) != 4:
        return summary()
    _, outfile = sound_env(work, 30)
    img = initrd(work, dict(files, gsttest=GSTTEST))
    said = arm64_boot(cores=2, mib=1024, initrd=img, timeout=900,
                      append="run=" + ",".join(["gsttest"] + STEPS),
                      extra=["-global", "virtio-mmio.force-legacy=false"] +
                      AUDIO)
    _, code, body = program(said, "gsttest")
    got = parts(body)
    check(f"gsttest runs every step from the initrd ({sorted(got)})",
          len(got) == 6)

    out = got.get("elements", "")
    missing = re.findall(r"element (\S+) missing", out)
    found = re.findall(r"element (\S+) found", out)
    check(f"every element WebKit's media player asks for is built in "
          f"({len(found)} found, missing: {missing})", found and not missing)

    for name in ("h264.mp4", "vp9.webm"):
        want = host_frames(files[name])
        mine = re.findall(r"^frame \d+ ([0-9a-f]{32})",
                          got.get("frames " + name, ""), re.M)
        first = next((i for i, (a, b) in enumerate(zip(want, mine))
                      if a != b), None)
        check(f"{name}: every frame decoded on the phone is the host's "
              f"FFmpeg's ({len(mine)} frames, host {len(want)}; first "
              f"difference {'none' if first is None else first})",
              len(want) == 50 and mine == want)

    frames, md5 = host_pcm(files["tone.mkv"])
    m = re.search(r"^pcm (\d+) ([0-9a-f]{32})", got.get("pcm tone.mkv", ""),
                  re.M)
    check(f"the FLAC tone decodes to the host's samples, byte for byte "
          f"({m.group(0) if m else 'nothing'}; host {frames} {md5})",
          m is not None and int(m.group(1)) == frames and m.group(2) == md5)

    frames, _ = host_pcm(files["opus.webm"])
    m = re.search(r"^pcm (\d+) ", got.get("pcm opus.webm", ""), re.M)
    n = int(m.group(1)) if m else 0
    check(f"Opus decodes to as many frames as the host's, give or take its "
          f"decoder's delay ({n}, host {frames})", abs(n - frames) <= 960)

    played = got.get("play tone.mkv", "")
    check(f"the tone played through threadosaudiosink to its end "
          f"({'played' if 'played' in played else 'not played'})",
          re.search(r"^played$", played, re.M) is not None)
    tone = raw_tone(outfile)
    check(f"and out of the phone's speaker comes 1000 Hz for about two "
          f"seconds (peak, ms, Hz: {tone})", tone is not None and
          abs(tone[2] - 1000) <= 20 and 1600 <= tone[1] <= 2400)
    check(f"gsttest ends with code 0 ({code})", code == 0)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
