#!/usr/bin/env python3
"""ThreadOS smoke test: ThreadOS mobile - the ARM64 kernel as a phone.

QEMU's virt machine with what a phone has: a screen (ramfb, which the
kernel sets up at 720x1280 through fw_cfg), a touchscreen (a virtio
tablet over virtio-mmio), a clock (the PL031, at a fixed date) and the
initrd with the fonts (through fw_cfg). On them the first shell of
ThreadOS mobile (kernelarm64/mobile.c), drawn by the PC's own graphics
code. Everything is held to something outside the kernel:

  while it starts, the screen is the PC's loading screen - black, the
  mark, the bar (kernel/bootsplash.c, built for both)

  the screen is the size QEMU shows, and the lock screen is the
  desktop's aurora pixel for pixel (ptest's own arithmetic of it), the
  time on it the clock QEMU was given plus the time since it started,
  the date that day as Python's calendar names it - read glyph by glyph
  against FreeType at the screen's two-to-one size

  each touch QEMU is sent arrives where it was sent - the kernel says
  every finger's down and up in logical pixels, and they are the ones
  the test chose

  a swipe up from the lock screen opens home, whose tiles are the
  colours mobile.c gives them and whose labels read as they are named;
  a tap on About opens it, and what it says - the processor, the cores,
  the memory, the screen, the touchscreen - is what QEMU was told to
  build (-cpu, -smp, -m, the ramfb size, the tablet's name)

  a swipe up from the bottom edge goes home from an app; Draw draws
  under the finger - the stroke's middle in the accent, a point beside
  it still the page's white - and Clear wipes it; Clock shows the time
  to the second

Run on its own or through scripts/run_tests.py.
"""

import datetime
import os
import re
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, noarm64, aurora_px, Phone, RTC_BASE,
                   _kernel_define)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
SRC = "kernelarm64/mobile.c"


def define(name):
    return _kernel_define(SRC, name)


def colour(name):
    v = int(re.search(r"#define\s+%s\s+0x([0-9A-Fa-f]{6})" % name,
                      open(os.path.join(ROOT, SRC)).read()).group(1), 16)
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


def apps():
    """mobile.c's apps: (name, colour), in their order on home"""
    src = open(os.path.join(ROOT, SRC)).read()
    # (the grid's own array; the dock's comes after it)
    src = re.search(r"apps\[\] = \{(.*?)\n\};", src, re.S).group(1)
    out = []
    for name, c in re.findall(r'\{\s*"(\w+)",\s*APP_\w+,\s*0x([0-9A-Fa-f]{6})'
                              r'\s*\}', src):
        v = int(c, 16)
        out.append((name, ((v >> 16) & 255, (v >> 8) & 255, v & 255)))
    return out


WHITE = (255, 255, 255)
K = 2                                      # panel pixels to a logical one


# When the kernel said what its clock read at start, by this side's
# clock, and what it read: QEMU sets the PL031 to -rtc base once the
# machine is built, seconds after this side starts counting, so the
# reference is the kernel's own word - itself held to -rtc base below
REF = {}


def clock_at(t):
    """What the PL031 said at the moment t of this side's clock: what it
    said at start, plus the seconds since"""
    return REF["read"] + datetime.timedelta(seconds=t - REF["at"])


def near_times(t, fmt, slack=3):
    """The clock's reading when the picture was taken (at t - reading it
    back afterwards takes seconds), and a few seconds either side: the
    kernel draws on the turn, and the serial port is read every 0.3 s"""
    now = clock_at(t)
    return {(now + datetime.timedelta(seconds=d)).strftime(fmt)
            for d in range(-slack - 1, slack + 1)}


def main():
    reason = noarm64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    print("--- ThreadOS mobile ---")
    q = Phone(cores=4, mib=1024)
    try:
        # While it starts: the PC's loading screen (kernel/bootsplash.c),
        # at the places its numbers put things on this screen
        bs = "kernel/bootsplash.c"
        d = _kernel_define(bs, "SPLASH_LOGO_D")
        bar_w = min(Phone.LOGICAL[0] - 64, _kernel_define(bs, "SPLASH_BAR_W"))
        bar_h = _kernel_define(bs, "SPLASH_BAR_H")
        cy = Phone.LOGICAL[1] // 2
        mark_row = (cy - _kernel_define(bs, "SPLASH_LOGO_DY")) * K
        end_x = ((Phone.LOGICAL[0] - bar_w) // 2 + bar_w - bar_h - 2) * K
        mid = (cy + _kernel_define(bs, "SPLASH_BAR_DY") + bar_h // 2) * K
        track = tuple((255 * _kernel_define(bs, "SPLASH_TRACK_A") + 127)
                      // 255 for _ in range(3))
        splash = False
        end = time.time() + 60
        while time.time() < end and not splash and \
                "[mobile] lock screen" not in q.serial():
            try:
                w, h, at = q.screen_raw()
            except Exception:
                time.sleep(0.2)
                continue
            splash = ((w, h) == (720, 1280) and
                      all(at(x, y) == (0, 0, 0) for x, y in
                          ((2, 2), (w - 3, 2), (2, h - 3), (w - 3, h - 3)))
                      and any(at(x, mark_row) == WHITE for x in
                              range((w - d * K) // 2, (w + d * K) // 2))
                      and at(end_x, mid) in (track, WHITE))
            time.sleep(0.1)
        check("while it starts, the phone shows the PC's loading screen - "
              "black, the mark, the bar", splash)
        q.wait_serial("Clock:", timeout=90)
        REF["at"] = time.time()
        out = q.wait_serial("[mobile] lock screen", timeout=90)
        # As many files as the image's own listing says, and its size
        with open(ARM64_INITRD, "rb") as fh:
            image = fh.read()
        files = struct.unpack_from("<I", image, 4)[0]
        check("the kernel finds the initrd through fw_cfg (%s)"
              % said_line(out, "initrd:"),
              said_line(out, "initrd:") == "initrd: %d files, %d bytes, "
              "through fw_cfg" % (files, len(image)))
        check("and starts the screen, upright, at two to one (%s)"
              % said_line(out, "Screen:"),
              "Screen: ramfb, 720x1280, two pixels to one" in out)
        check("and the touchscreen, a virtio tablet (%s)"
              % said_line(out, "Touch:"),
              "Touch: QEMU Virtio Tablet, X 0..32767, Y 0..32767" in out)
        base = datetime.datetime.fromisoformat(RTC_BASE)
        m = re.search(r"Clock: (\d+)-(\d+)-(\d+) (\d+):(\d+):(\d+) UTC", out)
        said = datetime.datetime(*map(int, m.groups())) if m else None
        check("the clock reads the date QEMU was given (%s, given %s)"
              % (said, base),
              said is not None and
              0 <= (said - base).total_seconds() <= 30)
        REF["read"] = said or base

        time.sleep(1)
        w, h, at = q.screen_raw()
        shot = time.time()
        check("QEMU shows a screen of 720x1280 (%dx%d)" % (w, h),
              (w, h) == (720, 1280))
        if (w, h) != (720, 1280):
            return summary()
        lw = w // K

        def aurora(x, y):
            return aurora_px(x, y, w, h)
        band = [(x, y) for y in range(560, 1100, 37) for x in range(5, w, 41)]
        off = [(x, y) for x, y in band if at(x, y) != aurora(x, y)]
        check("the lock screen is the desktop's aurora, pixel for pixel "
              "(%d of %d points differ)" % (len(off), len(band)), not off)
        t = q.find_ui(0, w, define("MOB_LOCK_TIME_Y") * K,
                      px=define("MOB_LOCK_TIME_PX") * K, face="bold",
                      under=aurora, fg=WHITE, screen=(w, h, at))
        want = near_times(shot, "%H:%M")
        check("the time on it is the clock's (%r, the clock %s)"
              % (t, sorted(want)), t in want)
        d = q.find_ui(0, w, define("MOB_LOCK_DATE_Y") * K,
                      px=define("MOB_LOCK_DATE_PX") * K, face="sans",
                      under=aurora, fg=WHITE, screen=(w, h, at))
        day = clock_at(shot)
        want_d = "%s, %d %s" % (day.strftime("%A"), day.day,
                                day.strftime("%B"))
        check("and the date that day as Python names it (%r)" % d,
              d == want_d)

        # --- the finger
        seen = len(q.serial())
        q.touch([(180, 560), (180, 480), (180, 380), (180, 300)])
        out = q.wait_serial("[mobile] home", timeout=20)[seen:]
        check("a touch arrives where it was sent: down at 180,560 and up "
              "at 180,300 (%s; %s)" % (said_line(out, "[touch] down"),
                                       said_line(out, "[touch] up")),
              "[touch] down 180,560" in out and "[touch] up 180,300" in out)
        check("a swipe up opens the phone: home",
              "[mobile] home (swipe up)" in out)

        time.sleep(0.5)
        w, h, at = q.screen_raw()
        gx, step = define("MOB_GRID_X"), define("MOB_GRID_STEP")
        gy, tile = define("MOB_GRID_Y"), define("MOB_TILE")
        bad = []
        for k, (name, c) in enumerate(apps()):
            x, y = gx + k * step, gy
            got_c = at((x + 4) * K, (y + tile // 2) * K)
            label = q.find_ui((x - 12) * K, (x + tile + 12) * K,
                              (y + tile + 18) * K,
                              px=define("MOB_LABEL_PX") * K, face="sans",
                              under=aurora, fg=WHITE, screen=(w, h, at))
            if got_c != c or label != name:
                bad.append((name, got_c, c, label))
        check("home has the apps mobile.c names, each its colour, each "
              "labelled (wrong: %s)" % (bad or "none"),
              len(apps()) >= 3 and not bad)

        # --- About: what QEMU built
        seen = len(q.serial())
        q.tap(gx + tile // 2, gy + tile // 2)
        out = q.wait_serial("[mobile] About", timeout=20)[seen:]
        check("a tap on the first tile opens About",
              "[mobile] About (About)" in out)
        time.sleep(0.5)
        w, h, at = q.screen_raw()
        rows = {}
        for i in range(5):
            y = (define("MOB_INFO_Y") + i * define("MOB_INFO_STEP")) * K
            rows[i] = q.find_ui(w // 2, w, y, px=define("MOB_ROW_PX") * K,
                                face="sans", fg=WHITE, screen=(w, h, at))
        want_rows = ["Arm Cortex-A72", "4", "1024 MiB", "720 x 1280",
                     "QEMU Virtio Tablet"]
        check("About says what QEMU was told to build - -cpu cortex-a72, "
              "-smp 4, -m 1024, the screen, the tablet (%s)"
              % [rows[i] for i in range(5)],
              [rows[i] for i in range(5)] == want_rows)

        # --- home from the edge, then Draw
        seen = len(q.serial())
        q.touch([(180, 630), (180, 560), (180, 470)])
        out = q.wait_serial("[mobile] home", timeout=20)[seen:]
        check("a swipe up from the bottom edge goes home from an app",
              "[mobile] home (swipe up from the edge)" in out)
        seen = len(q.serial())
        q.tap(gx + 2 * step + tile // 2, gy + tile // 2)
        out = q.wait_serial("[mobile] Draw", timeout=20)[seen:]
        check("a tap on the third tile opens Draw", "[mobile] Draw" in out)
        q.touch([(60, 200), (120, 260), (180, 320), (240, 260), (300, 200)])
        time.sleep(0.5)
        w, h, at = q.screen_raw()
        accent = colour("ACCENT")
        mids = [(90, 230), (150, 290), (210, 290), (270, 230)]
        drawn = [at(x * K, y * K) for x, y in mids]
        beside = at(180 * K, 200 * K)
        check("Draw draws under the finger: the stroke's middles in the "
              "accent %s (%s), a point beside it white (%s)"
              % (accent, drawn, beside),
              all(p == accent for p in drawn) and beside == WHITE)
        q.tap(lw - 40, define("MOB_TITLE_Y") - 6)
        time.sleep(0.5)
        w, h, at = q.screen_raw()
        check("and Clear wipes the page",
              all(at(x * K, y * K) == WHITE for x, y in mids))

        # --- Clock, to the second
        q.touch([(180, 630), (180, 560), (180, 470)])
        q.wait_serial("swipe up from the edge", timeout=20)
        seen = len(q.serial())
        q.tap(gx + step + tile // 2, gy + tile // 2)
        q.wait_serial("[mobile] Clock", timeout=20)
        time.sleep(1.5)
        w, h, at = q.screen_raw()
        shot = time.time()
        c = q.find_ui(0, w, 330 * K, px=64 * K, face="bold", fg=WHITE,
                      screen=(w, h, at))
        want = near_times(shot, "%H:%M:%S")
        check("Clock shows the time to the second (%r)" % c, c in want)
    finally:
        q.close()
    return summary()


if __name__ == "__main__":
    sys.exit(main())
