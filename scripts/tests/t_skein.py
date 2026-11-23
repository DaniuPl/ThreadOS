#!/usr/bin/env python3
"""ThreadOS test: Skein, the music player.

user64/gskein.c (docs/weave/canvas/Skein.dc.html) plays the music in
~/Music through FFmpeg and the system's sound stream. The machine boots
from an ext2 disk whose home has an album of three FLAC songs - sine
tones this script makes, put into FLAC by the host's ffmpeg, the first
carrying a picture of three colours - an MP3 at 44.1 kHz in an album of
its own, a playlist, and a WAV outside the Music folder. QEMU writes
what the sound card plays to a WAV file. Under Weave, on a screen the
size of the design's:

  the library: four songs in two albums and the playlist; the album
  woven from its picture's three colours, the other from its name
  Listen now is the album that came last: its title and its songs on
  the screen, its cover the picture's colours - the cloth exactly, the
  rows and columns over it
  Play: the first song, its title on the shuttle and in Playing, the
  other two next on the thread; it plays to its end, and every frame
  Skein handed the sound card is the tone this script made (SHA-256 of
  the PCM, decoded by Skein's FFmpeg, against the bytes before the
  host's encoder had them); the next follows by itself, is paused -
  nothing out of the card - and plays again from where it was, and
  still loses no frame; the third, and the thread runs out. Out of the
  card came 440, 660 and 880 Hz, in that order
  Albums, the MP3's album, its song: a click three quarters along the
  thread goes there - only the last quarter is heard - at 550 Hz, so
  44.1 kHz was made 48 kHz and not played fast
  Artists, Songs, Playlists (and a playlist's songs), From Files and
  Radio show what each should

Run on its own or through scripts/run_tests.py.
"""

import hashlib
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, LOOK, _kernel_define, UiFont)

L = LOOK
N = theme("night")
SRC = "user64/gskein.c"
SW, SH = 1440, 900                 # the design's screen
RATE = 48000
T = 1900000000                     # the files' time on the disk
CLOTH, ROWS, COLS = 0xB23D8C, 0x5B7CFA, 0xF5A524

ALBUM = "Warp and weft"
BAND = "The Selvedge Quartet"
SONGS = [("Cast on", 440, 5.0, "01 Cast on.flac"),
         ("The shuttle", 660, 3.0, "02 The shuttle.flac"),
         ("Selvedge", 880, 3.0, "03 Selvedge.flac")]
MP3_HZ, MP3_S = 550, 6.0


def d(name):
    return _kernel_define(SRC, name)


def pcm(hz, secs, right_gain):
    """A tone, 16-bit stereo at 48 kHz; the right channel quieter, so
    that swapped channels would not hash the same"""
    out = bytearray()
    for i in range(int(secs * RATE)):
        v = math.sin(2 * math.pi * hz * i / RATE)
        out += struct.pack("<hh", int(round(12000 * v)),
                           int(round(12000 * right_gain * v)))
    return bytes(out)


def png(path, w, h, bands):
    """A picture of horizontal bands: [(rgb, rows)]"""
    raw = bytearray()
    for rgb, rows in bands:
        line = b"\x00" + bytes([rgb >> 16, (rgb >> 8) & 255, rgb & 255]) * w
        raw += line * rows
    def chunk(t, data):
        c = t + data
        return struct.pack(">I", len(data)) + c + \
            struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n" +
                 chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                 chunk(b"IDAT", zlib.compress(bytes(raw))) +
                 chunk(b"IEND", b""))


def ffmpeg(args):
    r = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y"] +
                       args, capture_output=True)
    return r.returncode == 0


def make_home(tree, work):
    """The home: the album, the MP3, a playlist, a WAV elsewhere; the
    SHA-256 of each FLAC's PCM"""
    home = os.path.join(tree, "home/admin")
    alb = os.path.join(home, "Music", BAND, ALBUM)
    os.makedirs(alb)
    os.makedirs(os.path.join(home, "Music/Ola"))
    os.makedirs(os.path.join(home, "Documents"))
    cover = os.path.join(work, "cover.png")
    png(cover, 100, 100, [(CLOTH, 60), (ROWS, 25), (COLS, 15)])
    sums, ok = {}, True
    for k, (title, hz, secs, name) in enumerate(SONGS):
        raw = pcm(hz, secs, 0.5 + 0.1 * k)
        sums[title] = hashlib.sha256(raw).hexdigest()
        rawf = os.path.join(work, "t%d.raw" % k)
        open(rawf, "wb").write(raw)
        args = ["-f", "s16le", "-ar", str(RATE), "-ac", "2", "-i", rawf]
        if k == 0:
            args += ["-i", cover, "-map", "0:a", "-map", "1:v", "-c:v", "png",
                     "-disposition:v", "attached_pic"]
        args += ["-c:a", "flac", "-metadata", "title=" + title,
                 "-metadata", "artist=" + BAND, "-metadata", "album=" + ALBUM,
                 "-metadata", "date=2026", "-metadata", "track=%d/3" % (k + 1),
                 os.path.join(alb, name)]
        ok = ffmpeg(args) and ok
    ok = ffmpeg(["-f", "lavfi", "-i", "sine=frequency=%d:sample_rate=44100:"
                 "duration=%g" % (MP3_HZ, MP3_S), "-ac", "1", "-c:a",
                 "libmp3lame", "-b:a", "128k", "-metadata", "title=Field notes",
                 "-metadata", "artist=Ola", "-metadata",
                 "album=Field recordings", "-metadata", "date=2025",
                 os.path.join(home, "Music/Ola/Field notes.mp3")]) and ok
    ok = ffmpeg(["-f", "lavfi", "-i", "sine=frequency=330:sample_rate=48000:"
                 "duration=1", "-c:a", "pcm_s16le",
                 os.path.join(home, "Documents/voice memo.wav")]) and ok
    with open(os.path.join(home, "Music/Evening.m3u"), "w") as fh:
        fh.write("#EXTM3U\nOla/Field notes.mp3\n%s/%s/%s\n"
                 "../Documents/voice memo.wav\n" % (BAND, ALBUM, SONGS[2][3]))
    # the album came after the MP3: Listen now is it
    for root, dirs, files in os.walk(home):
        for f in files:
            p = os.path.join(root, f)
            os.utime(p, (T, T + 3600 if ALBUM in p else T))
    return ok, sums


def main():
    print("--- Skein ---")
    work = tempfile.mkdtemp()
    try:
        tree = os.path.join(work, "tree")
        os.makedirs(os.path.join(tree, "tmp"))
        ok, sums = make_home(tree, work)
        check("the host's ffmpeg makes the songs (FLAC with a picture, MP3, "
              "WAV)", ok)
        img = os.path.join(work, "disk.img")
        subprocess.run(["truncate", "-s", "96M", img], check=True)
        subprocess.run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", "4096",
                        "-L", "ThreadOS", "-d", tree, img], check=True)
        wav = os.path.join(work, "out.wav")
        q = Qemu(KERNEL, INITRD, img, sata=True, ram_mib=1024, audio=wav,
                 edid=(SW, SH), serial=os.path.join(work, "s.log"))
        try:
            run(q, sums, wav)
        finally:
            q.close()
    finally:
        subprocess.run(["rm", "-rf", work])
    return summary()


# --- the sound out of the card -----------------------------------------

def wav_head(path):
    with open(path, "rb") as fh:
        head = fh.read(44)
    return struct.unpack_from("<I", head, 24)[0], \
        struct.unpack_from("<H", head, 22)[0]


def wav_at(path):
    """Bytes of samples written so far"""
    return max(os.path.getsize(path) - 44, 0)


def wav_left(path, a=0, b=None):
    rate, ch = wav_head(path)
    with open(path, "rb") as fh:
        fh.seek(44 + a)
        body = fh.read(-1 if b is None else max(b - a, 0))
    n = len(body) // 2
    s = struct.unpack("<%dh" % n, body[:n * 2])
    return rate, s[0::ch]


def tone(seg, rate):
    """The frequency of a stretch, from the median gap between upward
    zero crossings; None when it is quiet"""
    if not seg or max(abs(v) for v in seg) < 1000:
        return None
    cross = [i for i in range(1, len(seg)) if seg[i - 1] <= 0 < seg[i]]
    if len(cross) < 4:
        return None
    gaps = sorted(cross[i] - cross[i - 1] for i in range(1, len(cross)))
    return rate / float(gaps[len(gaps) // 2])


def chunk_tones(path, a=0, b=None, chunk_ms=50):
    """The tone of each stretch of chunk_ms that sounds"""
    rate, left = wav_left(path, a, b)
    n = rate * chunk_ms // 1000
    out = []
    for i in range(0, len(left) - n, n):
        hz = tone(left[i:i + n], rate)
        if hz is not None:
            out.append(int(round(hz / 10.0) * 10))
    return out


def tones(path, a=0, b=None):
    """The tones heard, in order, each once: a tone that lasts three
    stretches of 50 ms at least - the one stretch where one song gives
    way to the next is neither"""
    runs = []
    for hz in chunk_tones(path, a, b):
        if runs and abs(runs[-1][0] - hz) <= 15:
            runs[-1][1] += 1
        else:
            runs.append([hz, 1])
    seq = []
    for hz, n in runs:
        if n >= 3 and (not seq or abs(seq[-1] - hz) > 15):
            seq.append(hz)
    return seq


def loud_ms(path, a, b=None):
    """How long it sounds between a and b, in ms"""
    rate, left = wav_left(path, a, b)
    n = rate // 100
    loud = sum(1 for i in range(0, len(left) - n, n)
               if max(abs(v) for v in left[i:i + n]) > 1000)
    return loud * 10


# --- the machine ---------------------------------------------------------

def run(q, sums, wav):
    check("the desktop came up", q.wait_desktop())
    W, H = q.size()
    check(f"the screen is the design's ({W}x{H})", (W, H) == (SW, SH))
    away = (W - 4, H // 2)
    q.click_desktop()
    check("'layout weave'", "saved" in q.sudo("set layout weave", "saved"))
    end = time.time() + 10
    while time.time() < end and "the Weave style" not in q.serial():
        time.sleep(0.3)
    acc = N["accent"]
    R = L["WEAVE_WIN_R"]

    def front(timeout=30):
        last = None
        end = time.time() + timeout
        while time.time() < end:
            q.mouse_goto(*away)
            _, _, at = q.screen_raw()
            got = None
            for y in range(0, H - 200):
                for x in range(0, W - 100, 2):
                    if at(x, y) == acc and at(x + 99, y) == acc and \
                            at(x, y + 2) == acc and at(x, y + 3) != acc:
                        got = (x - R, y)
                        break
                if got:
                    break
            if got and got == last:
                return got
            last = got
            time.sleep(0.5)
        return None

    def wait_after(seen, text, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            out = q.serial()[seen:]
            if text in out:
                return out
            time.sleep(0.2)
        return q.serial()[seen:]

    q.type_line("runw gskein")
    out = wait_after(0, "[skein] drawn", 90)
    m = re.search(r"\[skein\] drawn (\d+) (\d+)", out)
    cw = int(m.group(1)) if m else 0
    fw = front()
    check(f"Skein's window ({cw} wide, at {fw})", fw and cw == d("W1"))
    if not fw:
        return
    ox, oy = fw[0] + 1, fw[1] + L["WEAVE_TITLE_H"] + 1

    # --- the library --------------------------------------------------
    check("the library: four songs in two albums, and the playlist",
          "[skein] library 4 songs, 2 albums, 1 playlists" in out)
    m = re.search(r"\[skein\] album %s by %s: 3 songs, FLAC, woven (\w+) "
                  r"(\w+) (\w+) from its (\w+)" % (ALBUM, BAND), out)
    got = tuple(int(v, 16) for v in m.groups()[:3]) if m else None
    check(f"the album is woven from its picture's three colours, most first "
          f"({m.groups() if m else None})",
          m and m.group(4) == "picture" and got == (CLOTH, ROWS, COLS))
    check("the MP3's album, with no picture, from its name",
          re.search(r"\[skein\] album Field recordings by Ola: 1 songs, MP3, "
                    r"woven \w+ \w+ \w+ from its name", out) is not None)
    check("Listen now is the album that came last",
          "[skein] view Listen now: %s" % ALBUM in out)

    main_x = d("NAV_W") + 1 + d("MAIN_X")
    tx = main_x + d("COVER") + d("COVER_GAP")
    top = d("MAIN_Y") + d("COVER") - (105 + d("TITLE_PX"))
    title_base = top + 23 + d("TITLE_PX") * 864 // 1000
    table_y = d("MAIN_Y") + d("COVER") + 20
    rows_y = table_y + d("HEAD_H")
    cap = (d("ROW_PX") * 73 + 50) // 100
    row_base = (d("ROW_H") + cap) // 2
    title_x = main_x + d("COL_GAP") + d("NUM_W") + d("COL_GAP")

    def row_titles(n, scr, face="inter"):
        return [q.read_ui(ox + title_x, oy + rows_y + k * d("ROW_H") + row_base,
                          300, px=d("ROW_PX"), face=face, screen=scr)
                for k in range(n)]

    time.sleep(1)
    scr = q.screen_raw()
    title = q.read_ui(ox + tx, oy + title_base, 500, px=d("TITLE_PX"),
                      face="interb", screen=scr)
    shown = row_titles(3, scr)
    check(f"the album on the screen: its title ({title!r}) and its songs "
          f"({shown})", title == ALBUM and
          shown == [s[0] for s in SONGS])
    at = scr[2]
    cx, cy = ox + main_x, oy + d("MAIN_Y")
    cloth = at(cx + 10, cy + 10)
    row = at(cx + 10, cy + d("COVER") - 3)
    col = at(cx + 2, cy + 10)
    rgb = lambda v: (v >> 16, (v >> 8) & 255, v & 255)
    near = lambda p, c, a: all(abs(x - y) <= 2 for x, y in zip(
        p, [round(u * a + v * (1 - a)) for u, v in zip(rgb(c), rgb(CLOTH))]))
    check(f"its cover: the picture's cloth ({cloth}), its rows ({row}) and "
          f"columns ({col}) over it", cloth == rgb(CLOTH) and
          near(row, ROWS, 0.75) and near(col, COLS, 0.55))

    # --- playing it ------------------------------------------------------
    by = top + 23 + d("TITLE_PX") + 8 + 18 + 8 + 8
    seen = len(q.serial())
    start = wav_at(wav)
    q.click(ox + tx + 30, oy + by + d("BTN_H") // 2)
    q.mouse_goto(*away)
    out = wait_after(seen, "[skein] playing 1/3", 20)
    check("Play: the album from its first song",
          "[skein] playing 1/3 Cast on" in out)

    # the shuttle carries it, by the song's name
    loom = H - L["LOOM_GAP"] - L["LOOM_H"]
    sy = loom - L["SHT_LIFT"] - L["SHT_PLAY_H"]
    sx = (W - L["SHT_PLAY_W"]) // 2
    name = ""
    end = time.time() + 10
    while time.time() < end and name != "Cast on":
        name = q.read_ui(sx + 10 + L["SHT_ART"] + 12, sy + 23, 200, px=13,
                         face="interb")
        time.sleep(0.3)
    check(f"the shuttle carries the song's title ({name!r})", name == "Cast on")

    ax = cw - d("ASIDE_W")
    as_y = d("AS_PAD") + 15 + d("AS_GAP") + d("AS_COVER") + d("AS_GAP")
    wave_y = as_y + 40 + d("AS_GAP")
    ctl_y = wave_y + d("WAVE_H") + d("AS_GAP") - 10 + 15 + d("AS_GAP")
    q_y = ctl_y + d("CTL_D") + d("AS_GAP") + 12 + 15 + 8
    scr = q.screen_raw()
    playing = q.read_ui(ox + ax + d("AS_PAD"), oy + as_y + 17, 300,
                        px=d("NOW_PX"), face="interb", screen=scr)
    nexts = [q.read_ui(ox + ax + d("AS_PAD") + 20,
                       oy + q_y + k * d("Q_STEP") + 13, 280, px=d("Q_PX"),
                       face="inter", screen=scr) for k in range(2)]
    check(f"Playing: {playing!r}; next on the thread: {nexts}",
          playing == "Cast on" and nexts == ["The shuttle", "Selvedge"])

    out = wait_after(seen, "[skein] played Cast on", 40)
    m = re.search(r"\[skein\] played Cast on: (\d+) frames, sha256 (\w+)", out)
    check(f"it plays to its end, and every frame handed to the sound card "
          f"is the tone made here ({m.groups() if m else None})",
          m and int(m.group(1)) == int(SONGS[0][2] * RATE) and
          m.group(2) == sums["Cast on"])
    out = wait_after(seen, "[skein] playing 2/3", 10)
    check("the next follows by itself", "[skein] playing 2/3 The shuttle" in out)

    # paused from Playing, and played again
    mid = ax + d("ASIDE_W") // 2
    time.sleep(1)
    seen2 = len(q.serial())
    q.click(ox + mid, oy + ctl_y + d("CTL_D") // 2)
    q.mouse_goto(*away)
    out = wait_after(seen2, "[skein] paused at", 10)
    check("pause, in Playing", "[skein] paused at" in out)
    at_pause = wav_at(wav)
    time.sleep(2)
    rate, ch = wav_head(wav)
    quiet = loud_ms(wav, at_pause + rate * ch * 2 // 2, wav_at(wav))
    check(f"nothing out of the card while paused ({quiet} ms of sound)",
          quiet == 0)
    seen2 = len(q.serial())
    q.click(ox + mid, oy + ctl_y + d("CTL_D") // 2)
    q.mouse_goto(*away)
    out = wait_after(seen2, "[skein] played The shuttle", 30)
    m = re.search(r"\[skein\] played The shuttle: (\d+) frames, sha256 (\w+)",
                  out)
    check(f"it plays again from where it was, and loses no frame "
          f"({m.groups() if m else None})", "[skein] playing again at" in out
          and m and int(m.group(1)) == int(SONGS[1][2] * RATE) and
          m.group(2) == sums["The shuttle"])
    out = wait_after(seen, "[skein] the thread ran out", 30)
    m = re.search(r"\[skein\] played Selvedge: (\d+) frames, sha256 (\w+)", out)
    check("the third, and the thread runs out",
          m and m.group(2) == sums["Selvedge"] and
          "[skein] the thread ran out" in out)
    time.sleep(0.5)
    heard = tones(wav, start, wav_at(wav))
    check(f"out of the card: 440, 660 and 880 Hz, in that order ({heard})",
          len(heard) == 3 and all(abs(h - e) <= e * 0.02 for h, e in
                                  zip(heard, (440, 660, 880))))

    # --- Albums, the MP3, a seek -----------------------------------------
    def nav(i):
        y = d("NAV_TOP") + i * (d("NAV_H") + d("NAV_GAP")) + \
            (d("NAV_SPLIT") if i >= 4 else 0)
        s0 = len(q.serial())
        q.click(ox + d("NAV_SIDE") + 40, oy + y + d("NAV_H") // 2)
        q.mouse_goto(*away)
        return wait_after(s0, "[skein] view", 15)

    out = nav(1)
    check("Albums: both, by who made them",
          "[skein] view Albums: Field recordings | %s" % ALBUM in out)
    seen = len(q.serial())
    q.click(ox + main_x + d("GRID_COVER") // 2, oy + d("VIEW_TOP") + 40)
    out = wait_after(seen, "[skein] opened", 10)
    check("its cover opens the album", "[skein] opened Field recordings" in out)
    time.sleep(0.5)
    seen = len(q.serial())
    q.click(ox + title_x + 20, oy + rows_y + d("ROW_H") // 2)
    q.mouse_goto(*away)
    out = wait_after(seen, "[skein] playing 1/1", 15)
    check("its song plays", "[skein] playing 1/1 Field notes" in out)
    time.sleep(0.4)
    mark = wav_at(wav)
    wave_w = d("ASIDE_W") - 2 * d("AS_PAD")
    q.click(ox + ax + d("AS_PAD") + wave_w * 3 // 4,
            oy + wave_y + d("WAVE_H") // 2)
    q.mouse_goto(*away)
    out = wait_after(seen, "[skein] played Field notes", 30)
    m = re.search(r"\[skein\] to ([\d.]+) s of ([\d.]+)", out)
    check(f"a click three quarters along the thread goes there "
          f"({m.groups() if m else None})",
          m and abs(float(m.group(1)) / float(m.group(2)) - 0.75) < 0.02 and
          "[skein] played Field notes, after a seek" in out)
    time.sleep(0.5)
    heard_ms = loud_ms(wav, mark, wav_at(wav))
    rate, _ = wav_head(wav)
    each = sorted(chunk_tones(wav, mark, wav_at(wav)))
    hz = each[len(each) // 2] if each else 0
    check(f"only the last quarter is heard ({heard_ms} ms, not the "
          f"{int(MP3_S * 1000)} of the whole), at 550 Hz: 44.1 kHz made 48 "
          f"kHz ({hz})", 900 <= heard_ms <= 3500 and
          abs(hz - MP3_HZ) <= MP3_HZ * 0.02)

    # --- the other views ---------------------------------------------------
    out = nav(2)
    scr = q.screen_raw()
    first = q.read_ui(ox + main_x + 44, oy + d("VIEW_TOP") + 22, 300, px=14,
                      face="interb", screen=scr)
    check(f"Artists: who made them ({first!r})",
          "[skein] view Artists: Ola | %s" % BAND in out and first == "Ola")
    out = nav(3)
    time.sleep(0.5)
    scr = q.screen_raw()
    rows_y = d("VIEW_TOP") + d("HEAD_H")
    shown = row_titles(4, scr)
    check(f"Songs: all four, album by album ({shown})",
          "[skein] view Songs: 4 songs" in out and
          shown == ["Field notes", "Cast on", "The shuttle", "Selvedge"])
    out = nav(4)
    check("Playlists: the .m3u, three songs - one of them from Documents",
          "[skein] view Playlists: Evening (3)" in out)
    seen = len(q.serial())
    q.click(ox + main_x + 100, oy + d("VIEW_TOP") + d("LIST_H") // 2)
    q.mouse_goto(*away)
    out = wait_after(seen, "[skein] playlist", 10)
    time.sleep(0.5)
    shown = row_titles(3, q.screen_raw())
    check(f"a playlist's songs ({shown})",
          "[skein] playlist Evening: 3 songs" in out and
          shown == ["Field notes", "Selvedge", "voice memo"])
    out = nav(6)
    check("From Files: what sounds elsewhere in the home",
          "[skein] view From Files: voice memo" in out)
    nav(5)
    time.sleep(0.5)
    head = q.read_ui(ox + main_x, oy + d("MAIN_Y") + d("VIEW_PX"), 300,
                     px=d("VIEW_PX"), face="interb")
    check(f"Radio says what it is ({head!r})", head == "Radio")
    q.sendkey("alt-f4")
    time.sleep(1)


if __name__ == "__main__":
    sys.exit(main())
