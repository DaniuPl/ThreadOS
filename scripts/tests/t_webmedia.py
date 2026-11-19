#!/usr/bin/env python3
"""ThreadOS smoke test: a page's video and sound in Weft (64-bit kernel).

WebKit built with media (Makefile: ENABLE_VIDEO, ENABLE_MEDIA_SOURCE,
ENABLE_WEB_AUDIO, USE_GSTREAMER) plays it through GStreamer - its
demuxers and parsers, FFmpeg's decoders through gst-libav - in the web
process, paints the frames with the rest of the page, and plays the
sound through threadosaudiosink (apps/webkit/media/). Every file here
is made by FFmpeg on the host and served by the test, and each page is
opened in its own Weft and started by a click on it, as a person starts
one - a browser lets a page make sound only after one:

  <video> with H.264 in MP4 - what a page's video most often is - a
  second of red, one of green, one of blue: the screen shows each of
  them in turn where the page puts the video, in the colours FFmpeg on
  the host decodes the same frames to, give or take the conversion to
  RGB; the page's script says it ended, three seconds in, at the size
  FFmpeg says the video is

  WebKit's own controls on a video (controls): paused, the play button
  in its middle has its icon - drawn white over the red frame - and a
  click on it plays the video; with the pointer over it as it plays,
  the bar of buttons along its foot has theirs

  Media Source Extensions, which YouTube streams through: the page asks
  which types it can be fed (VP9 and Opus in WebM, H.264 and AAC in
  MP4), fetches the same colours as VP9 in WebM, appends them to a
  SourceBuffer and plays them - the three colours, in order, again

  <audio> with Opus in WebM: a 1000 Hz tone comes out of QEMU's sound
  card for about the two seconds it lasts, and the island at the top
  of the screen names the page playing it - not the web process,
  which has no window: Weft's, its parent's

  a tab whose page plays wears a speaker, where there was nothing while
  it was quiet; a click on it mutes the tab - 1000 Hz out of the card
  before, nothing after - and another lets it be heard again

  Weft tells the system what it plays (SYS64_NOW_PLAYING), and the
  island has pause for it: a click there pauses the page - nothing out
  of the card, and the island stays a while, naming the page, with play
  in pause's place - and another plays it again; paused and left, the
  island goes once that while is over

  Web Audio: an oscillator at 440 Hz started and stopped two seconds
  later by the page's script - 440 Hz out of the card for two seconds

Run on its own or through scripts/run_tests.py.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, make_iso64, no64, wav_tone, ROUTES,
                   island_name, island_np_x)
from t_webkit import find_box, wait_line, tell, Band, SRC
from ptest import _kernel_define as define

BUILD = os.path.dirname(os.path.abspath(KERNEL64 or "build/x"))
APPS = os.path.join(BUILD, "weft.tapp")
# The pages' background and a marker under the video, of colours no
# frame has: the marker is found exactly, and the video is where the
# page puts it from there (MARK_DY CSS pixels above it). All of it fits
# a phone's 360 CSS pixels across.
PAGE_BG = (0x20, 0x20, 0x28)
MARK = (0xC0, 0x80, 0x20)
COLOURS = ("c03030", "30a050", "3366cc")       # red, green, blue: a second each
NEAR = 14              # what the conversion to RGB may move a channel by
MARK_DY = 260

PAGE_HEAD = (
    '<!doctype html><html><head><meta charset="utf-8"><title>loading'
    '</title><style>html,body{margin:0;height:100%;background:#202028}'
    '#mark{position:absolute;left:20px;top:280px;width:40px;height:40px;'
    'background:#c08020}'
    'video{position:absolute;left:20px;top:20px;width:320px;'
    'height:240px}</style></head><body><div id="mark"></div>'
    # what a page does when it is done: with ?chain=a,b it goes on to
    # a.html?chain=b - the phone's test runs them all in one Weft
    '<script>function done() {'
    ' const c = (new URLSearchParams(location.search).get("chain") || "")'
    '   .split(",").filter(Boolean);'
    ' if (c.length) setTimeout(() => { location.href = "/" + c[0] +'
    '   ".html?chain=" + c.slice(1).join(","); }, 1500); }</script>')

# Each page says in its title when it can play, and plays when the page
# is clicked: a browser lets a page start sound only from a person's
# click or tap (WebKit's autoplay policy, as Safari's and Chrome's), and
# a video started by the test is seen from its first frame.
VIDEO = (PAGE_HEAD + """
<video id="v" src="/colours.mp4" muted playsinline preload="auto"></video>
<script>
const v = document.getElementById('v');
v.addEventListener('canplaythrough', () => { document.title = 'video ready'; },
                   { once: true });
document.addEventListener('click', () => { v.play(); });
v.addEventListener('playing', () => { document.title = 'playing'; });
v.addEventListener('ended', () => {
  document.title = 'ended ' + v.currentTime.toFixed(1) + ' ' +
                   v.videoWidth + 'x' + v.videoHeight; done(); });
v.addEventListener('error', () => {
  document.title = 'error ' + (v.error ? v.error.code : '?'); });
</script></body></html>""").encode()

# WebKit's own controls (controls), not the page's: a button with a
# play icon over the paused video, a bar of buttons when the pointer is
# over it playing. The icons are the theme's SVGs, typed by their names
# (libc's made-up /usr/share/mime/globs2).
CONTROLS = (PAGE_HEAD + """
<video id="v" src="/colours.mp4" controls muted preload="auto"></video>
<script>
const v = document.getElementById('v');
v.addEventListener('canplaythrough', () => { document.title = 'controls ready'; },
                   { once: true });
v.addEventListener('playing', () => { document.title = 'controls playing'; });
</script></body></html>""").encode()

TYPES = ['video/webm; codecs="vp9"', 'audio/webm; codecs="opus"',
         'video/mp4; codecs="avc1.42E01E"', 'audio/mp4; codecs="mp4a.40.2"']

MSE = (PAGE_HEAD + """
<video id="v" muted playsinline></video>
<script>
const v = document.getElementById('v');
const types = %s;
const said = 'types ' + types.map(t =>
  MediaSource.isTypeSupported(t) ? 'yes' : 'no').join(' ');
document.title = said;
const ms = new MediaSource();
v.src = URL.createObjectURL(ms);
ms.addEventListener('sourceopen', async () => {
  const sb = ms.addSourceBuffer(types[0]);
  const data = await (await fetch('/colours.webm')).arrayBuffer();
  sb.addEventListener('updateend', () => {
    ms.endOfStream();
    document.title = 'appended ' + sb.buffered.end(0).toFixed(1);
  }, { once: true });
  sb.appendBuffer(data);
});
document.addEventListener('click', () => { v.play(); });
v.addEventListener('ended', () => {
  document.title = 'mse ended ' + v.currentTime.toFixed(1) + ' ' +
                   v.videoWidth + 'x' + v.videoHeight; done(); });
v.addEventListener('error', () => {
  document.title = 'error ' + (v.error ? v.error.code : '?'); });
</script></body></html>""" % json.dumps(TYPES)).encode()

AUDIO = (PAGE_HEAD + """
<audio id="a" src="/tone.webm" preload="auto"></audio>
<script>
const a = document.getElementById('a');
a.addEventListener('canplaythrough', () => { document.title = 'audio ready'; },
                   { once: true });
document.addEventListener('click', () => {
  a.play().catch(e => { document.title = 'refused ' + e.name; }); });
a.addEventListener('playing', () => { document.title = 'audio playing'; });
a.addEventListener('ended', () => { document.title = 'audio ended';
                                     done(); });
a.addEventListener('error', () => {
  document.title = 'error ' + (a.error ? a.error.code : '?'); });
</script></body></html>""").encode()

# Twenty seconds of the tone, for a tab muted and heard again on its
# way, and paused and played again from the island
LONG = (PAGE_HEAD + """
<audio id="a" src="/long.webm" preload="auto"></audio>
<script>
const a = document.getElementById('a');
a.addEventListener('canplaythrough', () => { document.title = 'long ready'; },
                   { once: true });
document.addEventListener('click', () => { a.play(); }, { once: true });
a.addEventListener('playing', () => { document.title = 'long playing'; });
</script></body></html>""").encode()

WEBAUDIO = (PAGE_HEAD + """
<script>
// what happened, in the title, when it did not: the test says it
const said = what => { document.title = what; };
const c = new AudioContext();
said('webaudio ready');
document.addEventListener('click', () => {
  const o = c.createOscillator();
  const g = c.createGain();
  o.frequency.value = 440;
  g.gain.value = 0.5;
  o.connect(g).connect(c.destination);
  o.onended = () => { said('oscillator ended ' + c.sampleRate); done(); };
  c.resume().then(() => {
    said('oscillator ' + c.state);
    o.start();
    o.stop(c.currentTime + 2);
  }, e => said('resume failed ' + e));
}, { once: true });
</script></body></html>""").encode()


def ffmpeg(*args):
    return subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error",
                           "-y"] + list(args), capture_output=True)


def make_files(work):
    """The media, made by the host's FFmpeg: name -> path. The video is
    three seconds, a colour each, 320 x 240 at 25 frames a second."""
    files = {}
    cols = []
    for i, c in enumerate(COLOURS):
        cols += ["-f", "lavfi", "-i",
                 f"color=c=0x{c}:size=320x240:rate=25:duration=1"]
    concat = ["-filter_complex", "[0:v][1:v][2:v]concat=n=3:v=1:a=0[v]",
              "-map", "[v]"]
    tone = ["-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000:"
            "duration=2"]
    jobs = {
        "colours.mp4": cols + concat + ["-c:v", "libx264", "-pix_fmt",
                                        "yuv420p", "-g", "25",
                                        "-movflags", "+faststart"],
        "colours.webm": cols + concat + ["-c:v", "libvpx-vp9", "-b:v",
                                         "200k", "-deadline", "realtime",
                                         "-cpu-used", "8", "-g", "25"],
        "tone.webm": tone + ["-ac", "2", "-c:a", "libopus", "-b:a", "96k"],
        "long.webm": ["-f", "lavfi", "-i", "sine=frequency=1000:"
                      "sample_rate=48000:duration=20", "-ac", "2", "-c:a",
                      "libopus", "-b:a", "96k"],
    }
    for name, args in jobs.items():
        path = os.path.join(work, name)
        if ffmpeg(*args, path).returncode == 0:
            files[name] = path
    return files


def host_colour(path, at):
    """The middle pixel of the frame 'at' seconds in, as FFmpeg on the
    host decodes it and converts it to RGB"""
    raw = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error",
                          "-ss", str(at), "-i", path, "-frames:v", "1",
                          "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                         capture_output=True).stdout
    if len(raw) < 320 * 240 * 3:
        return None
    o = (120 * 320 + 160) * 3
    return tuple(raw[o:o + 3])


def host_size(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                          "-show_entries", "stream=width,height", "-of",
                          "csv=p=0:s=x", path], capture_output=True,
                         text=True).stdout.strip()
    return out


def close_to(a, b):
    return a is not None and b is not None and \
        all(abs(x - y) <= NEAR for x, y in zip(a, b))


def serve(name, blob, kind):
    """A file as a web server sends it: its type, and a part of it when
    a range is asked for, which is how GStreamer reads a file it plays"""
    def answer(h, body):
        rng = re.match(r"bytes=(\d+)-(\d*)$", h.headers.get("Range") or "")
        if not rng:
            return 200, [("Content-Type", kind),
                         ("Accept-Ranges", "bytes")], blob
        a = int(rng.group(1))
        b = int(rng.group(2)) if rng.group(2) else len(blob) - 1
        b = min(b, len(blob) - 1)
        if a >= len(blob):
            return 416, [("Content-Range", f"bytes */{len(blob)}")], b""
        return 206, [("Content-Type", kind), ("Accept-Ranges", "bytes"),
                     ("Content-Range", f"bytes {a}-{b}/{len(blob)}")], \
            blob[a:b + 1]
    ROUTES["/" + name] = answer


def open_page(q, port, path):
    """Weft started on a page of the test's: the serial port's length
    before it, or None when the page did not load"""
    q.click_desktop()
    time.sleep(1)
    seen = len(q.serial())
    q.type_line(f"run weft http://10.0.2.2:{port}{path}")
    done = wait_line(q, seen, "[weft] load finished", 300)
    if done is None:
        tell(q, seen)
    return seen, done


def close_weft(q, seen):
    q.sendkey("ctrl-w")
    wait_line(q, seen, "[weft] done, exit code", 60)
    time.sleep(1)


def video_origin(q, k=1, timeout=60):
    """The page's video box on the screen, found from the marker under
    it: (x, y) of its top left corner, or None. 'k' is the screen's
    pixels to a CSS pixel."""
    end = time.time() + timeout
    while time.time() < end:
        sw, sh, at = q.screen_raw()
        m = find_box(at, sw, sh, MARK)
        if m and m[2] - m[0] == 40 * k and m[3] - m[1] == 40 * k:
            return m[0], m[1] - MARK_DY * k
        time.sleep(1)
    return None


def colours_in_turn(q, x, y, want, timeout=60):
    """Reads the middle of the video until each wanted colour has been
    there, in order: what was seen, as one entry for each colour that
    showed (a frame each, not one frame repeated)"""
    seen, i = [], 0
    end = time.time() + timeout
    while i < len(want) and time.time() < end:
        _, _, at = q.screen_raw()
        got = at(x, y)
        if close_to(got, want[i]):
            seen.append(got)
            i += 1
        time.sleep(0.1)
    return seen


def click_spot(org, k=1):
    """Where the test clicks a page to start it: on the page, clear of
    the video and the marker (CSS 200, 300)"""
    return org[0] + 180 * k, org[1] + 280 * k


def video_page(q, port, files, name, path, ended):
    """A page whose video plays the three colours when clicked: they
    show in turn, in FFmpeg's colours, and the page says it ended"""
    want = [host_colour(files[name], t) for t in (0.5, 1.5, 2.5)]
    seen, done = open_page(q, port, path)
    check(f"{path}: Weft loads the page ({done})", done is not None)
    if done is None:
        return
    ready = wait_line(q, seen, "[weft] title video ready", 120)
    org = video_origin(q)
    check(f"the page is on the screen, its video ready to play ({ready}; "
          f"its video at {org})", ready is not None and org is not None)
    if ready is None or org is None:
        tell(q, seen)
        close_weft(q, seen)
        return
    q.click(*click_spot(org))
    shown = colours_in_turn(q, org[0] + 160, org[1] + 120, want)
    check(f"clicked, the video plays: its colours on the screen in turn, "
          f"as the host's FFmpeg decodes them ({shown}; FFmpeg {want})",
          len(shown) == 3)
    line = wait_line(q, seen, ended, 60)
    size = host_size(files[name])
    check(f"and the page's script hears it end, three seconds in, at the "
          f"video's size ({line}; FFmpeg {size})",
          line is not None and line.endswith(f" 3.0 {size}"))
    if line is None:
        tell(q, seen)
    close_weft(q, seen)


def white(at, x0, y0, x1, y1):
    """How many pixels of the box are nearly white - an icon's, over a
    video of red, green and blue"""
    return sum(1 for y in range(y0, y1) for x in range(x0, x1)
               if min(at(x, y)) >= 215)


def controls_page(q, port):
    """WebKit's own controls on a video: their icons drawn, their play
    button playing it, their bar of buttons over it as it plays"""
    seen, done = open_page(q, port, "/controls.html")
    ready = wait_line(q, seen, "[weft] title controls ready", 120)
    org = video_origin(q)
    check(f"/controls.html: the video with WebKit's controls loads ({ready})",
          ready is not None and org is not None)
    if org is None:
        tell(q, seen)
        close_weft(q, seen)
        return
    cx, cy = org[0] + 160, org[1] + 120
    n = 0
    end = time.time() + 30
    while n < 20 and time.time() < end:
        _, _, at = q.screen_raw()
        n = white(at, cx - 30, cy - 30, cx + 30, cy + 30)
        time.sleep(0.5)
    check(f"paused, the play button in its middle has its icon ({n} white "
          f"pixels; none without the icons)", n >= 20)
    q.click(cx, cy)
    line = wait_line(q, seen, "[weft] title controls playing", 30)
    check(f"a click on it plays the video ({line})", line is not None)
    q.mouse_goto(cx - 40, cy + 40)
    n = 0
    end = time.time() + 30
    while n < 20 and time.time() < end:
        q.mouse_goto(cx - 40 + int(time.time()) % 2, cy + 40)
        _, _, at = q.screen_raw()
        n = white(at, org[0] + 4, org[1] + 200, org[0] + 120, org[1] + 238)
        time.sleep(0.3)
    check(f"and with the pointer over it, the bar of buttons along its foot "
          f"({n} white pixels of its icons)", n >= 20)
    close_weft(q, seen)


def tab_speaker(q, org):
    """How many pixels of the first tab's speaker box are not the tab's
    own colour: none while its page is quiet"""
    band = Band(org[0] - 20, org[1] - 20)
    tw = band.tw(1)
    x0 = band.ox + 4 + tw - 2 - band.close_w - define(SRC, "SOUND_W")
    y0 = band.top + 3
    _, _, at = q.screen_raw()
    bg = at(x0, y0 + 1)
    return sum(1 for y in range(y0, band.top + band.tab_h)
               for x in range(x0, x0 + define(SRC, "SOUND_W"))
               if at(x, y) != bg), (x0 + 9, y0 + (band.tab_h - 3) // 2)


def wav_at(wav):
    """Where the sound QEMU writes has got to, in bytes of samples, and
    how many bytes a second is"""
    import struct
    with open(wav, "rb") as fh:
        head = fh.read(44)
    rate, ch = struct.unpack_from("<I", head, 24)[0], \
        struct.unpack_from("<H", head, 22)[0]
    return max(os.path.getsize(wav) - 44, 0), rate * ch * 2


def mute_page(q, port, wav):
    """A tab that plays sound wears a speaker; a click on it mutes the
    tab - silence out of the card - and another lets it be heard"""
    seen, done = open_page(q, port, "/long.html")
    ready = wait_line(q, seen, "[weft] title long ready", 120)
    org = video_origin(q)
    if ready is None or org is None:
        check(f"/long.html: the page is ready ({ready})", False)
        tell(q, seen)
        close_weft(q, seen)
        return
    quiet, _ = tab_speaker(q, org)
    q.click(*click_spot(org))
    line = wait_line(q, seen, "[weft] sound playing", 60)
    n, spot = 0, None
    end = time.time() + 20
    while line and n < 15 and time.time() < end:
        n, spot = tab_speaker(q, org)
    check(f"/long.html: as its sound plays, the tab wears a speaker ({n} "
          f"pixels of it; {quiet} while the page was quiet)",
          line is not None and quiet == 0 and n >= 15)
    if spot is None:
        close_weft(q, seen)
        return
    time.sleep(1)
    played, per_s = wav_at(wav)
    mark = len(q.serial())
    q.click(*spot)
    muted = wait_line(q, mark, "[weft] tab 1 muted", 20)
    at_mute, _ = wav_at(wav)
    time.sleep(2)
    hush = wav_tone_between(wav, at_mute + per_s // 2, wav_at(wav)[0])
    mark = len(q.serial())
    q.click(*spot)
    heard = wait_line(q, mark, "[weft] tab 1 heard", 20)
    at_back, _ = wav_at(wav)
    time.sleep(1.5)
    back = wav_tone_between(wav, at_back + per_s // 2, wav_at(wav)[0])
    before = wav_tone_between(wav, max(played - per_s, 0), played)
    check(f"a click on the speaker mutes the tab: 1000 Hz before ({before}), "
          f"nothing out of the card after ({muted}; {hush})",
          before is not None and abs(before[2] - 1000) <= 20 and
          muted is not None and hush is None)
    check(f"and another lets it be heard: 1000 Hz again ({heard}; {back})",
          heard is not None and back is not None and abs(back[2] - 1000) <= 20)
    island_play_pause(q, seen, wav)
    close_weft(q, seen)


def island_shown(q):
    """Whether the island's black is at the top centre"""
    _, _, at = q.screen_raw()
    return at(SCREEN_W // 2, ISL_TOP + 4) == ISL_BG


def np_bars(q):
    """How much of the island's play or pause looks like pause's two
    bars: the bright pixels down the middle of the right one, above and
    below its middle - five for pause; the play triangle narrows to its
    point there, so none for play"""
    _, _, at = q.screen_raw()
    x = island_np_x() + 15
    cy = ISL_TOP + ISL_PLAY_H // 2
    return sum(1 for y in (cy - 5, cy - 4, cy - 3, cy + 3, cy + 4)
               if min(at(x, y)) > 160)


def island_play_pause(q, seen, wav):
    """Weft tells the system what it plays; the island has play and
    pause for it, which go back to Weft and to the page"""
    said = wait_line(q, seen, "[weft] now playing: playing", 10)
    bars = 0
    end = time.time() + 10
    while said and bars < 5 and time.time() < end:
        bars = np_bars(q)
    check(f"Weft says it plays, and the island has pause for it ({said}; "
          f"{bars} of 5 pixels of its bar)", said is not None and bars == 5)
    spot = (island_np_x() + ISL_NP_W // 2, ISL_TOP + ISL_PLAY_H // 2)
    mark = len(q.serial())
    q.click(*spot)
    q.mouse_goto(400, 400)        # the pointer off the button it covers
    paused = wait_line(q, mark, "[weft] pause from the system: 1 media", 20)
    told = wait_line(q, mark, "[weft] now playing: paused", 20)
    at_pause, per_s = wav_at(wav)
    time.sleep(1.5)
    hush = wav_tone_between(wav, at_pause + per_s // 2, wav_at(wav)[0])
    shown = island_shown(q)
    name = island_name(q, np=True)
    bars = np_bars(q)
    check(f"a click on it pauses the page: nothing out of the card ({paused}; "
          f"{told}; {hush})", paused is not None and told is not None
          and hush is None)
    check(f"and the island stays a while, naming the page, with play in "
          f"place of pause ({shown}; {name!r}; {bars} pixels of the bar)",
          shown and name == "long playing" and bars == 0)
    mark = len(q.serial())
    q.click(*spot)
    q.mouse_goto(400, 400)
    played = wait_line(q, mark, "[weft] play from the system: 1 media", 20)
    told = wait_line(q, mark, "[weft] now playing: playing", 20)
    at_play, _ = wav_at(wav)
    time.sleep(1.5)
    back = wav_tone_between(wav, at_play + per_s // 2, wav_at(wav)[0])
    check(f"and another plays it again: 1000 Hz ({played}; {told}; {back})",
          played is not None and told is not None and back is not None
          and abs(back[2] - 1000) <= 20)
    if played is None or told is None:
        tell(q, mark)
    # Paused and left, the island goes once its while is over
    mark = len(q.serial())
    q.click(*spot)
    q.mouse_goto(400, 400)
    told = wait_line(q, mark, "[weft] now playing: paused", 20)
    t0 = time.time()
    while told and island_shown(q) and time.time() - t0 < ISL_NP_HOLD + 5:
        time.sleep(0.2)
    gone = time.time() - t0
    check(f"paused again and left, the island goes after {ISL_NP_HOLD} s "
          f"({told}; gone {gone:.1f} s on)", told is not None and
          ISL_NP_HOLD - 1 <= gone <= ISL_NP_HOLD + 3)


def wav_tone_between(wav, a, b):
    """wav_tone over the bytes of samples from a to b"""
    if b <= a:
        return None
    cut = wav + ".part"
    with open(wav, "rb") as fh:
        head = fh.read(44)
        fh.seek(44 + a)
        body = fh.read(b - a)
    with open(cut, "wb") as fh:
        fh.write(head + body)
    try:
        return wav_tone(cut)
    finally:
        os.unlink(cut)


def mse_page(q, port, files):
    seen, done = open_page(q, port, "/mse.html")
    types = wait_line(q, seen, "[weft] title types", 60)
    check(f"Media Source Extensions take VP9 and Opus in WebM, H.264 and "
          f"AAC in MP4 ({types})",
          types == "[weft] title types yes yes yes yes")
    app = wait_line(q, seen, "[weft] title appended", 120)
    check(f"the WebM appended to a SourceBuffer is three seconds of video "
          f"({app})", app == "[weft] title appended 3.0")
    org = video_origin(q) if app else None
    if org is None:
        tell(q, seen)
        close_weft(q, seen)
        return
    want = [host_colour(files["colours.webm"], t) for t in (0.5, 1.5, 2.5)]
    q.click(*click_spot(org))
    shown = colours_in_turn(q, org[0] + 160, org[1] + 120, want)
    check(f"and clicked, plays: its colours in turn ({shown}; FFmpeg "
          f"{want})", len(shown) == 3)
    line = wait_line(q, seen, "[weft] title mse ended", 60)
    size = host_size(files["colours.webm"])
    check(f"to its end ({line}; FFmpeg {size})",
          line is not None and line.endswith(f" 3.0 {size}"))
    close_weft(q, seen)


def sound_page(q, port, wav, path, ready, started, ended, hz):
    """A page that plays a tone of 'hz' for two seconds when clicked:
    out of the card"""
    seen, done = open_page(q, port, path)
    line = wait_line(q, seen, ready, 120)
    org = video_origin(q)
    check(f"{path}: the page is ready to play ({line})",
          line is not None and org is not None)
    if line is None or org is None:
        tell(q, seen)
        close_weft(q, seen)
        return
    before = max(os.path.getsize(wav) - 44, 0) if os.path.exists(wav) else 0
    q.click(*click_spot(org))
    line = wait_line(q, seen, started, 60)
    check(f"clicked, its sound starts ({line})", line is not None)
    # the island at the top names what plays: the web process has no
    # window, Weft's - named after the page - is its parent's
    want = started[len("[weft] title "):]
    names = set()
    end = time.time() + 2
    while line and want not in names and time.time() < end:
        names.add(island_name(q, np=True))
    check(f"and the island names the page playing it, not the web process "
          f"({names}, want {want!r})", want in names)
    line = wait_line(q, seen, ended, 60)
    check(f"and ends ({line})", line is not None)
    if line is None:
        tell(q, seen)
    time.sleep(1)
    tone = wav_tone(wav, before)
    check(f"out of the HDA comes {hz} Hz for about two seconds "
          f"(peak, ms, Hz: {tone})", tone is not None and
          abs(tone[2] - hz) <= hz // 50 and 1600 <= tone[1] <= 2600)
    close_weft(q, seen)


def main():
    print()
    print("--- A page's video and sound in Weft (64-bit kernel) ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    if not os.path.exists(APPS):
        print("SKIPPED: no weft.tapp - make webkit")
        return summary()
    if not shutil.which("ffmpeg"):
        print("SKIPPED: no FFmpeg on this machine to make the media with")
        return summary()

    work = tempfile.mkdtemp()
    try:
        files = make_files(work)
        check(f"the host's FFmpeg makes the media ({sorted(files)})",
              len(files) == 4)
        kinds = {"mp4": "video/mp4", "webm": "video/webm"}
        for name, path in files.items():
            serve(name, open(path, "rb").read(),
                  "audio/webm" if name.startswith(("tone", "long")) else
                  kinds[name.rsplit(".", 1)[1]])
        for path, body in (("/video.html", VIDEO), ("/mse.html", MSE),
                           ("/audio.html", AUDIO),
                           ("/webaudio.html", WEBAUDIO),
                           ("/controls.html", CONTROLS),
                           ("/long.html", LONG)):
            ROUTES[path] = (lambda b: lambda h, body: (
                200, [("Content-Type", "text/html; charset=utf-8")], b))(body)
        port, stop = start_http_server()
        iso = os.path.join(work, "t.iso")
        wav = os.path.join(work, "out.wav")
        try:
            check("the disc with the kernel and Weft builds",
                  make_iso64(os.path.join(work, "iso"), iso, graphics=True,
                             tapps=[APPS]))
            q = Qemu(KERNEL, INITRD, cdrom=iso, audio=wav, cpus=2,
                     serial=os.path.join(work, "serial.log"),
                     qemu="qemu-system-x86_64", ram_mib=2048)
            try:
                q.wait_serial("thread64>", timeout=120)
                q.wait_serial("[net] DHCP", timeout=60)
                q.type_line("desktop")
                q.wait_serial("The desktop is starting", timeout=30)
                check("the desktop comes up", q.wait_desktop())
                q.mouse_home()
                video_page(q, port, files, "colours.mp4", "/video.html",
                           "[weft] title ended")
                controls_page(q, port)
                mse_page(q, port, files)
                sound_page(q, port, wav, "/audio.html",
                           "[weft] title audio ready",
                           "[weft] title audio playing",
                           "[weft] title audio ended", 1000)
                mute_page(q, port, wav)
                sound_page(q, port, wav, "/webaudio.html",
                           "[weft] title webaudio ready",
                           "[weft] title oscillator running",
                           "[weft] title oscillator ended", 440)
            finally:
                q.close()
        finally:
            stop()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
