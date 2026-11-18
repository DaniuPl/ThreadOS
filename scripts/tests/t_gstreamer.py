#!/usr/bin/env python3
"""ThreadOS smoke test: GStreamer, as WebKit will play pages' media.

GStreamer 1.28 and FFmpeg built for ThreadOS (third_party/gstreamer,
the Makefile's GST_BUILD), every plugin built in and registered at
gst_init (apps/webkit/media/gststatic.c), the sound going out through
threadosaudiosink (apps/webkit/media/gstthreadossink.c) - run here on
their own, by gsttest (libc/tests/gsttest.c), before WebKit stands on
them.

The files are made here, by FFmpeg on the host: H.264 in MP4 and VP9 in
WebM - what YouTube sends - a tone in FLAC in Matroska, and Opus in
WebM. ThreadOS fetches them and gsttest over HTTP (gsttest is tens of
megabytes, too big for the initrd), then decodes them, and what it
prints is held to FFmpeg on the host:

  every element WebKit's media player asks for is there;
  each video frame, decoded through avdec and converted to I420, is the
  frame FFmpeg's framemd5 hashes - the same MD5 for every frame, in
  order (H.264 and VP9 decode to the same bits everywhere);
  the FLAC tone decoded to 16-bit stereo is FFmpeg's, byte for byte;
  the Opus decodes to as many frames as the host's, give or take the
  decoder's own delay;
  played through threadosaudiosink, the tone comes out of QEMU's HDA as
  1000 Hz for about the two seconds it lasts - the sink's stream begun,
  fed and ended from GStreamer's own threads.
"""

import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, wav_tone, start_http_server, SERVED

BUILD = os.path.dirname(os.path.abspath(KERNEL64 or "build/x"))
GSTTEST = os.path.join(BUILD, "user64", "gsttest.elf")


def ffmpeg(*args):
    return subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error",
                           "-y"] + list(args), capture_output=True)


def make_files(work):
    """The test's media, made by the host's FFmpeg: name -> path"""
    files = {}
    src = ["-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25:duration=2"]
    tone = ["-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000:"
            "duration=2"]
    jobs = {
        "h264.mp4": src + ["-c:v", "libx264", "-pix_fmt", "yuv420p",
                           "-g", "25", "-bf", "2"],
        "vp9.webm": src + ["-c:v", "libvpx-vp9", "-b:v", "400k",
                           "-deadline", "realtime", "-cpu-used", "8"],
        "tone.mkv": tone + ["-ac", "2", "-c:a", "flac"],
        "opus.webm": tone + ["-ac", "2", "-c:a", "libopus", "-b:a", "96k"],
    }
    for name, args in jobs.items():
        path = os.path.join(work, name)
        if ffmpeg(*args, path).returncode == 0:
            files[name] = path
    return files


def host_frames(path):
    """FFmpeg's MD5 of each frame, decoded and converted to I420"""
    out = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error",
                          "-i", path, "-map", "0:v", "-pix_fmt", "yuv420p",
                          "-f", "framemd5", "-"], capture_output=True,
                         text=True).stdout
    return [l.split(",")[-1].strip() for l in out.splitlines()
            if l and not l.startswith("#")]


def host_pcm(path):
    """FFmpeg's sound, as 16-bit stereo: frames, MD5"""
    raw = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error",
                          "-i", path, "-map", "0:a", "-ac", "2", "-f",
                          "s16le", "-"], capture_output=True).stdout
    return len(raw) // 4, hashlib.md5(raw).hexdigest()


def said(q, cmd, timeout):
    """What gsttest printed for 'cmd', its lines as the console ends them
    (\\n\\r) made plain"""
    return q.command(cmd, "[gsttest] done", timeout=timeout).replace("\r", "")


def main():
    print("--- GStreamer ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    if not os.path.exists(GSTTEST):
        print("SKIPPED: no gsttest - make gstreamer, then "
              "make " + GSTTEST)
        return summary()
    if not shutil.which("ffmpeg"):
        print("SKIPPED: no FFmpeg on this machine to make the media with")
        return summary()

    work = tempfile.mkdtemp()
    try:
        files = make_files(work)
        check(f"the host's FFmpeg makes the test's media ({sorted(files)})",
              len(files) == 4)
        SERVED["/gsttest"] = open(GSTTEST, "rb").read()
        for name, path in files.items():
            SERVED["/" + name] = open(path, "rb").read()
        port, stop = start_http_server()
        disk = os.path.join(work, "disk.img")
        with open(disk, "wb") as fh:
            fh.truncate(96 * 1024 * 1024)
        wav = os.path.join(work, "out.wav")
        q = Qemu(KERNEL, INITRD, disk, serial=os.path.join(work, "s.log"),
                 cpus=2, append="nodesktop", audio=wav,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("The 64-bit system is ready", timeout=90)
            q.wait_serial("[net] DHCP", timeout=60)
            got = []
            for name in ["gsttest"] + sorted(files):
                out = q.command(f"run httpget 10.0.2.2 {port} /{name} {name}",
                                "[httpget] done", timeout=180)
                got.append("[httpget] done, exit code 0" in out)
            check("gsttest and the media reach ThreadOS's disk", all(got))

            out = said(q, "run gsttest elements", 120)
            missing = re.findall(r"element (\S+) missing", out)
            found = re.findall(r"element (\S+) found", out)
            check(f"every element WebKit's media player asks for is built "
                  f"in and registered ({len(found)} found, missing: "
                  f"{missing})", found and not missing)

            for name in ("h264.mp4", "vp9.webm"):
                want = host_frames(files[name])
                out = said(q, f"run gsttest frames {name}", 300)
                mine = re.findall(r"^frame \d+ ([0-9a-f]{32})", out, re.M)
                first = next((i for i, (a, b) in enumerate(zip(want, mine))
                              if a != b), None)
                check(f"{name}: every frame decoded on ThreadOS is the "
                      f"host's FFmpeg's ({len(mine)} frames, host "
                      f"{len(want)}; first difference "
                      f"{'none' if first is None else first})",
                      len(want) == 50 and mine == want)

            frames, md5 = host_pcm(files["tone.mkv"])
            out = said(q, "run gsttest pcm tone.mkv", 120)
            m = re.search(r"^pcm (\d+) ([0-9a-f]{32})", out, re.M)
            check(f"the FLAC tone decodes to the host's samples, byte for "
                  f"byte ({m.group(0) if m else 'nothing'}; host {frames} "
                  f"{md5})", m is not None and int(m.group(1)) == frames
                  and m.group(2) == md5)

            frames, _ = host_pcm(files["opus.webm"])
            out = said(q, "run gsttest pcm opus.webm", 120)
            m = re.search(r"^pcm (\d+) ", out, re.M)
            n = int(m.group(1)) if m else 0
            check(f"Opus decodes to as many frames as the host's, give or "
                  f"take its decoder's delay ({n}, host {frames})",
                  abs(n - frames) <= 960)

            before = os.path.getsize(wav) - 44 if os.path.exists(wav) else 0
            out = said(q, "run gsttest play tone.mkv", 120)
            tone = wav_tone(wav, max(before, 0))
            check(f"played through threadosaudiosink to its end "
                  f"({'played' if 'played' in out else 'not played'})",
                  "\nplayed" in out)
            check(f"and out of the HDA comes 1000 Hz for about two seconds "
                  f"(peak, ms, Hz: {tone})", tone is not None and
                  abs(tone[2] - 1000) <= 20 and 1600 <= tone[1] <= 2400)
        finally:
            q.close()
            stop()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
