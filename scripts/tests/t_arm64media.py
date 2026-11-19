#!/usr/bin/env python3
"""ThreadOS smoke test: a page's video and sound in Weft on the phone.

t_webmedia's pages on the ARM64 kernel: WebKit built for aarch64 with
media (`make weft-arm64`), GStreamer and FFmpeg's aarch64 code playing
it, the frames painted into the page at the phone's 200%, the sound
through threadosaudiosink to the virtio-sound speaker - SDL's disk
driver writing what it plays to a file. One Weft goes through the four
pages in turn (each goes on to the next when it is done), each
started by a tap on it, as a person starts a page's video or sound:

  <video>, H.264 in MP4: its three colours on the phone's screen in
  turn, where the page puts the video, as FFmpeg on the host decodes
  them; it ends three seconds in, at the video's size

  Media Source Extensions: the types YouTube asks for are taken, the
  same colours as VP9 in WebM appended to a SourceBuffer and played

  <audio>, Opus in WebM, then Web Audio's oscillator: out of the
  speaker come 1000 Hz and then 440 Hz, about two seconds each
"""

import os
import re
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, noarm64, Phone, ARM64_INITRD, ROUTES,
                   start_http_server)
from t_arm64sound import AUDIO, RATE, sound_env
from t_arm64weft import wait_line
import t_webmedia as wm

WEFT = os.path.join(os.path.dirname(ARM64_INITRD), "aarch64", "weft.tapp")
K = 2                    # the phone's 200%: the panel's pixels to a CSS one


def tones(path):
    """The speaker's file (16-bit stereo at RATE) as the sounds in it:
    (milliseconds, hertz) for each loud stretch, the stretches parted by
    a fifth of a second or more of quiet"""
    if not os.path.exists(path):
        return []
    body = open(path, "rb").read()
    n = len(body) // 4
    left = struct.unpack("<%dh" % (n * 2), body[:n * 4])[0::2]
    if not left:
        return []
    peak = max(abs(v) for v in left)
    if peak < 200:
        return []
    gap = RATE // 5
    out, start, last = [], None, None
    for i, v in enumerate(left):
        if abs(v) > peak // 4:
            if start is None:
                start = i
            elif i - last > gap:
                out.append((start, last))
                start = i
            last = i
    if start is not None:
        out.append((start, last))
    found = []
    for a, b in out:
        seg = left[a:b + 1]
        cross = [i for i in range(1, len(seg)) if seg[i - 1] <= 0 < seg[i]]
        if len(cross) < 3:
            continue
        gaps = sorted(cross[i] - cross[i - 1] for i in range(1, len(cross)))
        found.append(((b - a + 1) * 1000.0 / RATE,
                      RATE / float(gaps[len(gaps) // 2])))
    return found


def start(q, ready, seen):
    """When the page says it is ready, a tap on it - as a person starts
    a page's video or sound: the video's place on the screen, or None"""
    line = wait_line(q, seen, ready, 300)
    org = wm.video_origin(q, K, timeout=180) if line else None
    check(f"the page is on the phone's screen, ready to play ({line}; its "
          f"video at {org}, at the phone's 200%)",
          line is not None and org is not None)
    if org is not None:
        x, y = wm.click_spot(org, K)
        q.tap(x // K, y // K)
    return org


def video(q, seen, files, name, org, ended):
    """The video's colours in turn on the screen, and its end, said"""
    want = [wm.host_colour(files[name], t) for t in (0.5, 1.5, 2.5)]
    shown = wm.colours_in_turn(q, org[0] + 160 * K, org[1] + 120 * K, want,
                               timeout=180) if org else []
    check(f"tapped, the video plays: its colours on the screen in turn, as "
          f"the host's FFmpeg decodes them ({shown}; FFmpeg {want})",
          len(shown) == 3)
    line = wait_line(q, seen, ended, 180)
    size = wm.host_size(files[name])
    check(f"and the page hears it end, three seconds in, at the video's "
          f"size ({line}; FFmpeg {size})",
          line is not None and line.endswith(f" 3.0 {size}"))


def main():
    print("--- A page's video and sound in Weft on the phone ---")
    why = noarm64()
    if why:
        print("SKIPPED: " + why)
        return summary()
    if not os.path.exists(WEFT):
        print("SKIPPED: no aarch64 weft.tapp (make weft-arm64)")
        return summary()
    import shutil
    if not shutil.which("ffmpeg"):
        print("SKIPPED: no FFmpeg on this machine to make the media with")
        return summary()

    work = tempfile.mkdtemp()
    files = wm.make_files(work)
    check(f"the host's FFmpeg makes the media ({sorted(files)})",
          len(files) == 3)
    if len(files) != 3:
        return summary()
    kinds = {"mp4": "video/mp4", "webm": "video/webm"}
    for name, path in files.items():
        wm.serve(name, open(path, "rb").read(),
                 "audio/webm" if name.startswith("tone") else
                 kinds[name.rsplit(".", 1)[1]])
    for path, body in (("/video.html", wm.VIDEO), ("/mse.html", wm.MSE),
                       ("/audio.html", wm.AUDIO),
                       ("/webaudio.html", wm.WEBAUDIO)):
        ROUTES[path] = (lambda b: lambda h, body: (
            200, [("Content-Type", "text/html; charset=utf-8")], b))(body)
    _, outfile = sound_env(work, 120)
    port, stop = start_http_server()
    url = f"http://10.0.2.2:{port}/video.html?chain=mse,audio,webaudio"
    q = Phone(cores=4, mib=2048, append="run=weft," + url,
              extra=["-fw_cfg", "name=opt/threados/apps/weft,file=" + WEFT,
                     "-netdev", "user,id=n0",
                     "-device", "virtio-net-device,netdev=n0",
                     "-global", "virtio-mmio.force-legacy=false"] + AUDIO)
    try:
        q.wait_serial("[mobile] lock screen", timeout=180)
        q.touch([(180, 560), (180, 420), (180, 300)])
        seen = 0
        done = wait_line(q, seen, "[weft] load finished", 600)
        check(f"Weft loads the video's page ({done})",
              done is not None and "/video.html" in done)
        org = start(q, "[weft] title video ready", seen)
        video(q, seen, files, "colours.mp4", org, "[weft] title ended")

        types = wait_line(q, seen, "[weft] title types", 180)
        check(f"Media Source Extensions take VP9 and Opus in WebM, H.264 "
              f"and AAC in MP4 ({types})",
              types == "[weft] title types yes yes yes yes")
        org = start(q, "[weft] title appended", seen)
        app = wait_line(q, seen, "[weft] title appended", 10)
        check(f"the WebM appended to a SourceBuffer is three seconds of "
              f"video ({app})", app == "[weft] title appended 3.0")
        video(q, seen, files, "colours.webm", org, "[weft] title mse ended")

        for ready, begun, end in (
                ("audio ready", "audio playing", "audio ended"),
                ("webaudio ready", "oscillator running", "oscillator ended")):
            start(q, "[weft] title " + ready, seen)
            line = wait_line(q, seen, "[weft] title " + begun, 180)
            check(f"tapped, the page's sound starts ({line})",
                  line is not None)
            line = wait_line(q, seen, "[weft] title " + end, 120)
            check(f"and ends ({line})", line is not None)
        time.sleep(2)
        got = tones(outfile)
        check(f"out of the speaker come 1000 Hz and then 440 Hz, about two "
              f"seconds each (ms, Hz: {got})",
              len(got) == 2 and
              abs(got[0][1] - 1000) <= 20 and 1600 <= got[0][0] <= 2600 and
              abs(got[1][1] - 440) <= 9 and 1600 <= got[1][0] <= 2600)
    finally:
        q.close()
        stop()
    return summary()


if __name__ == "__main__":
    sys.exit(main())
