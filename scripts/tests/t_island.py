#!/usr/bin/env python3
"""ThreadOS smoke test: the island at the top of the screen.

A black capsule at the top centre grows into what is going on and
shrinks away after (kernel/desktop.c). Two things go on there:

  how loud: the loudness keys, the wheel over the island, a click or a
  drag along its slider, a click on its speaker - and a change made
  anywhere else, here the kernel shell's 'volume' command. Every time
  the island's number and the length of its filled slider are read off
  the screen, and the kernel itself is asked ('volume' on the serial
  port) whether the loudness really is what the island says. A
  notification on the island gives way to a loudness key and comes
  back after it (t_notify has the rest of notifications)

  what is playing: gmedia plays a WAV made here - four seconds of a
  sine at 24000 of 32767, then three of silence. While it plays the
  island names it ("Media: tone.wav", its window's title) and its four
  bars stand as high as the samples are loud: the height the test
  works out from the WAV's own peak while the tone plays, the height
  of silence after it - the samples, not a decoration. When the
  stream ends the island goes, and a click on it while it plays
  opens the loudness

Run on its own or through scripts/run_tests.py.
"""

import io
import math
import os
import re
import shutil
import struct
import sys
import tempfile
import time
import wave

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64

RATE = 48000
TONE_S, QUIET_S, AMP = 4, 3, 24000


def kernel_volume(q):
    """What the sound driver itself says: (percent, muted)"""
    out = q.command("volume", "steps in the codec", timeout=20)
    m = re.search(r"volume: (\d+)%( \(muted\))?, \d+ steps", out)
    return (int(m.group(1)), bool(m.group(2))) if m else (None, None)


def wait_for(fn, want, timeout=10):
    """Polls fn() until it returns want; the last value seen"""
    deadline = time.time() + timeout
    got = fn()
    while got != want and time.time() < deadline:
        time.sleep(0.3)
        got = fn()
    return got


def fill_near(q, px):
    """The slider's filled length, polled until it is px (to a pixel):
    the frame the number was read from may still have the slider on its
    way to its new length - the last length seen"""
    deadline = time.time() + 10
    fill = island_slider(q)
    while abs(fill - px) > 1 and time.time() < deadline:
        time.sleep(0.3)
        fill = island_slider(q)
    return fill


def island_shown(q, screen=None):
    """Whether the island's black is at the top centre"""
    _, _, at = screen or q.screen_raw()
    return at(SCREEN_W // 2, ISL_TOP + 4) == ISL_BG


def loudness_part(q):
    bx = (SCREEN_W - ISL_VOL_W) // 2 + ISL_BAR_X
    cy = ISL_TOP + ISL_VOL_H // 2

    def said():
        return island_volume(q)

    check("at the start the island is not there", not island_shown(q))

    q.mouse_goto(400, 300)
    q.sendkey("volumeup")
    got = wait_for(said, "85%")
    fill = fill_near(q, 85 * ISL_BAR_W // 100)
    check(f"Volume Up: the island says 85% ('{got}'), its slider filled "
          f"{fill} of {ISL_BAR_W} px", got == "85%" and
          abs(fill - 85 * ISL_BAR_W // 100) <= 1)
    check("and the driver is at 85%", kernel_volume(q) == (85, False))

    for _ in range(3):
        q.sendkey("volumedown")
    got = wait_for(said, "70%")
    fill = fill_near(q, 70 * ISL_BAR_W // 100)
    check(f"Volume Down three times: 70% ('{got}'), filled {fill} px",
          got == "70%" and abs(fill - 70 * ISL_BAR_W // 100) <= 1)

    q.sendkey("audiomute")
    got = wait_for(said, "Off")
    fill = fill_near(q, 0)
    check(f"Mute: the island says Off ('{got}') and its slider is empty "
          f"({fill} px)", got == "Off" and fill == 0)
    check("and the driver is muted", kernel_volume(q) == (70, True))
    q.sendkey("audiomute")
    got = wait_for(said, "70%")
    check(f"Mute again: back to 70% ('{got}')", got == "70%")

    t0 = time.time()
    gone = wait_for(lambda: island_shown(q), False, timeout=15)
    check(f"after a moment the island goes "
          f"({time.time() - t0:.1f} s after the last key)", gone is False)

    # A change made elsewhere: the shell's command, on the serial port
    q.command("volume 40", "volume:")
    got = wait_for(said, "40%")
    check(f"'volume 40' typed in the kernel's shell: the island says so "
          f"('{got}')", got == "40%")

    # The wheel over the island, while it is there
    q.mouse_goto(SCREEN_W // 2, cy)
    q.mouse_wheel(2)
    got = wait_for(said, "50%")
    check(f"two notches of the wheel over it: 50% ('{got}')", got == "50%")
    check("and the driver agrees", kernel_volume(q) == (50, False))

    # A click along the slider, and a drag
    q.click(bx + ISL_BAR_W // 4, cy)
    got = wait_for(said, "25%")
    fill = fill_near(q, 25 * ISL_BAR_W // 100)
    check(f"a click a quarter of the way along the slider: 25% ('{got}', "
          f"filled {fill} px)", got == "25%" and
          abs(fill - 25 * ISL_BAR_W // 100) <= 1)
    check("and the driver is at 25%", kernel_volume(q) == (25, False))
    q.mouse_goto(bx + ISL_BAR_W // 4, cy)
    q.mouse_button(True)
    q.mouse_goto(bx + ISL_BAR_W * 3 // 4, cy)
    got = wait_for(said, "75%")
    held = got
    q.mouse_button(False)
    check(f"dragged to three quarters: 75% while held ('{held}')",
          held == "75%")
    check("and the driver is at 75% once let go",
          kernel_volume(q) == (75, False))
    check("and the setting keeps it",
          "volume = 75" in q.command("set", "slower", timeout=10))

    # The speaker: silence, and back
    q.click((SCREEN_W - ISL_VOL_W) // 2 + ISL_ICON_X, cy)
    got = wait_for(said, "Off")
    check(f"a click on the speaker mutes ('{got}')",
          got == "Off" and kernel_volume(q) == (75, True))
    q.click((SCREEN_W - ISL_VOL_W) // 2 + ISL_ICON_X, cy)
    got = wait_for(said, "75%")
    check(f"and another brings the sound back ('{got}')",
          got == "75%" and kernel_volume(q) == (75, False))
    q.mouse_goto(400, 400)

    # News waits for the loudness: a notification shown, a key pressed,
    # and the notification back once the loudness has gone
    wait_for(lambda: island_shown(q), False, timeout=15)
    q.command("notify Waiting", "thread64")
    got = wait_for(lambda: island_note(q), ("Shell", "Waiting"))
    check(f"a notification grows the island ({got})",
          got == ("Shell", "Waiting"))
    q.sendkey("volumedown")
    got = wait_for(said, "70%")
    check(f"a loudness key takes the island over ('{got}')", got == "70%")
    got = wait_for(lambda: island_note(q), ("Shell", "Waiting"), timeout=10)
    check(f"and the notification comes back after it ({got})",
          got == ("Shell", "Waiting"))


def tone_wav():
    frames = []
    for i in range(RATE * TONE_S):
        v = int(AMP * math.sin(2 * math.pi * 440 * i / RATE))
        frames.append(struct.pack("<hh", v, v))
    frames.append(b"\0\0\0\0" * (RATE * QUIET_S))
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(frames))
    peak = max(abs(int(AMP * math.sin(2 * math.pi * 440 * i / RATE)))
               for i in range(RATE // 10))
    return buf.getvalue(), peak


def playing_part(work):
    data, peak = tone_wav()
    SERVED["/tone.wav"] = data
    loud, quiet = island_level_px(peak), island_level_px(0)
    port, stop = start_http_server()
    disk = os.path.join(work, "disk.img")
    with open(disk, "wb") as fh:
        fh.write(b"\0" * DISK_SIZE)
    q = Qemu(KERNEL, INITRD, disk=disk, sata=True, cpus=2,
             serial=os.path.join(work, "serial.log"),
             audio=os.path.join(work, "out.wav"), qemu="qemu-system-x86_64")
    seen = []                          # (seconds, name, levels)
    try:
        q.wait_desktop()
        q.wait_serial("[net] DHCP", timeout=60)
        out = q.command(f"run httpget 10.0.2.2 {port} /tone.wav tone.wav",
                        "[httpget] done", timeout=60)
        check("tone.wav reaches the ThreadOS disk",
              "[httpget] done, exit code 0" in out)
        q.mouse_goto(400, 400)
        q.type_line("run gmedia tone.wav")
        t0 = time.time()
        clicked = None
        while time.time() - t0 < 40:
            scr = q.screen_raw()
            name = island_name(q, screen=scr)
            lev = island_levels(q, screen=scr) if name else []
            seen.append((time.time() - t0, name, lev))
            # Once the silence has been seen, the island is asked for
            # the loudness - the stream is still going
            if (clicked is None and name and lev == [quiet] * HDA_LEVELS
                    and any(l == [loud] * HDA_LEVELS for _, _, l in seen)):
                q.click(SCREEN_W // 2, ISL_TOP + ISL_PLAY_H // 2)
                clicked = wait_for(lambda: island_volume(q), "80%", 5)
                q.mouse_goto(400, 400)
            if seen[-1][1] == "" and any(s[1] for s in seen) and \
                    not island_shown(q):
                break
            time.sleep(0.2)
    finally:
        q.close()
        stop()

    names = {s[1] for s in seen if s[1]}
    check(f"while gmedia plays, the island names its window ({names})",
          names == {"Media: tone.wav"})
    during = [s for s in seen if s[1]]
    check(f"while the tone plays, every bar is {loud} px - its samples "
          f"peak at {peak} of 32767 ({[s[2] for s in during[:6]]})",
          any(s[2] == [loud] * HDA_LEVELS for s in during))
    first_loud = next((i for i, s in enumerate(during)
                       if s[2] == [loud] * HDA_LEVELS), None)
    check(f"and in the silence after it, every bar is {quiet} px",
          first_loud is not None and
          any(s[2] == [quiet] * HDA_LEVELS for s in during[first_loud:]))
    check(f"a click on it while it plays shows the loudness ('{clicked}')",
          clicked == "80%")
    check("when the stream ends, the island goes",
          bool(seen) and seen[-1][1] == "" and during and
          seen[-1][0] > during[-1][0])


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    work = tempfile.mkdtemp()
    try:
        q = Qemu(KERNEL, INITRD, serial=os.path.join(work, "s1.log"),
                 audio=os.path.join(work, "a1.wav"),
                 qemu="qemu-system-x86_64")
        try:
            q.wait_desktop()
            # the bar's first three seconds are over
            wait_for(lambda: island_shown(q), False, 10)
            loudness_part(q)
        finally:
            q.close()
        playing_part(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
