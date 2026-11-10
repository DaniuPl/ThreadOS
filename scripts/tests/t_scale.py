#!/usr/bin/env python3
"""ThreadOS smoke test: the panel's own resolution, and a scale drawn
sharp.

A desktop laid out in pixels was right while a screen was 640x480. A
laptop's panel is 1920x1080 on fifteen inches, and there two things
have to be true at once: the desktop has to use the panel's own mode -
anything else the panel scales, and a scaled picture is a soft one -
and it has to come out large enough to read. The second used to be
pixel doubling: everything drawn small and every pixel put down as a
square of four. Now the compositor draws AT the panel's size - every
edge worked out in the panel's pixels, every letter rendered from its
outline one and a half or two times larger - and only the layout is in
the smaller, logical pixels.

What is asserted, against things that do not come from the system:

  with the screen saying what it is (EDID, as QEMU's VGA does), the
  desktop starts in that mode - the screen QEMU shows is that size,
  and the aurora covers all of it, painted at that size - and at the
  hundred dots an inch that EDID gives, 'Auto' means no scale at all.
  GRUB sets a mode of its own first; the kernel takes it over
  the 'resolution' setting wins over what the screen says - changed
  while the desktop runs, kept for the next start, and 'native' gives
  the screen's own mode back

  with nothing said (as on a board whose card this system has no
  driver for), a panel of 1080 rows gets one and a half: the desktop
  is laid out at 1280x720 and drawn at 1920x1080
  and it is drawn, not blown up: a window's title, and the number in
  the calculator's display, are FreeType's letters at one and a half
  times their size, pixel for pixel - which no enlargement of the
  small letters could be
  the pointer still lands where the layout says: clicked keys of the
  calculator add up, typed ones too

  at two, the counter-check that it is not doubling: in a window's
  title, the panel's pixels are not equal in squares of four

  and changed while it runs, from Appearance: the desktop takes up one
  and a half without a restart, a program that was open is drawn again
  at the new size - its title and its numbers FreeType's at one and a
  half - and still answers its keys; 'Auto' puts it back

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary
from ptest import _kernel_define as define

DARK = theme("dark")
C = "user/gcalc.c"


def set_as_admin(q, name, value, timeout=30):
    """'sudo set' in the kernel's shell, the password given when it is
    asked for - in a signed-in admin's session it is not. True once the
    shell says the value was saved."""
    start = len(q.serial())
    q.type_line("sudo set %s %s" % (name, value))
    asked = False
    end = time.time() + timeout
    while time.time() < end:
        fresh = q.serial()[start:]
        if re.search(r"%s = \S+ \(saved\)" % re.escape(name), fresh):
            return True
        if "password:" in fresh and not asked:
            q.type_line("thread")
            asked = True
        time.sleep(0.3)
    return False


def edge_spots(w, h):
    """A few places down the right-hand edge, where nothing of the
    desktop stands - no card of the profile setup or the lock, no dock,
    no icon - at any size"""
    return [(w - 8, h // 2), (w - 8, h // 4), (w - 30, h * 3 // 4)]


def veiled(q, timeout=90):
    """Polls until the desktop is up and waiting for a person: the
    profile setup and the lock both dim the whole background the same way
    (as t_lock.py reads it), at whatever size and scale the desktop has.
    From then on the keys are the desktop's."""
    end = time.time() + timeout + q.boot_grace()
    while time.time() < end:
        w, h, at = q.screen_raw()
        spots = edge_spots(w, h)
        if w >= 640 and all(
                near(at(x, y), gfx_mix((0, 0, 0), aurora_px(x, y, w, h), 90),
                     4) for x, y in spots):
            return True
        time.sleep(0.5)
    return False


def signed_in(q, timeout=60):
    """Polls until the desktop shows its own background, unveiled, at the
    size it now has"""
    end = time.time() + timeout
    while time.time() < end:
        w, h, at = q.screen_raw()
        spots = edge_spots(w, h)
        if all(at(x, y) == aurora_px(x, y, w, h) for x, y in spots):
            return True
        time.sleep(0.5)
    return False


def enter(q, fresh):
    """Through what the desktop starts on: a profile to make on a fresh
    disk, the lock on one that has a profile. Qemu.wait_desktop reads
    those off the screen at the scale of 100, and here the scale is the
    very thing that changes - so the keys a person would press are
    pressed once the screen shows the desktop waiting for them."""
    check("the desktop starts, %s" % ("on the profile setup" if fresh
                                       else "on its lock"), veiled(q))
    if fresh:
        # The profile Qemu.wait_desktop makes: admin, password 'thread'
        q.type_line("admin", enter=False)
        q.sendkey("tab")
        q.type_line("Test Admin", enter=False)
        q.sendkey("tab")
        q.type_line("thread", enter=False)
        q.sendkey("tab")
        q.type_line("thread", enter=False)
        q.sendkey("tab")
        q.sendkey("tab")
        q.sendkey("ret")
    else:
        q.type_line("thread")
    check("and is signed in to", signed_in(q))


def boot(disk, slog, edid, settings=(), fresh=True):
    """A machine started into its desktop. With settings, they are made
    from the shell and the machine is started again on the same disk, so
    that its desktop starts with them."""
    q = Qemu(KERNEL, INITRD, disk, serial=slog, edid=edid)
    enter(q, fresh)
    # The mode GRUB started the kernel in, said at boot. On this card
    # the kernel takes it over (kernel/video.c), so it is only where the
    # desktop came from - worth seeing when a check below fails.
    said = re.search(r"Framebuffer: [^\r\n]*|loader gave no framebuffer",
                     q.serial())
    print("      (%s)" % (said.group(0) if said else "no framebuffer"))
    if settings:
        q.mouse_home()
        q.click_desktop()                    # the keys to the shell
        for key, value in settings:
            check("the shell saves %s = %s" % (key, value),
                  set_as_admin(q, key, value))
        q.close()
        q = Qemu(KERNEL, INITRD, disk, serial=slog, edid=edid)
        enter(q, False)
    q.mouse_home()
    q.mouse_goto(300, 200)
    time.sleep(1)
    return q


def mode(q):
    """What the machine says it draws at: logical size, scale, panel"""
    said = q.command("screen", "frames", timeout=10)
    m = re.search(r"mode:\s+(\d+)x(\d+), [^,]+, scale (\d+)%, panel (\d+)x(\d+)",
                  said)
    return tuple(int(v) for v in m.groups()) if m else None


def settle(q, done, timeout=20):
    """Asks the machine what it draws at, and QEMU what it shows, until
    done(mode, w, h) holds or the time is up: a setting is taken up
    within a second or so, later on a loaded host. The last answers
    either way, the screen with them."""
    end = time.time() + timeout
    while True:
        m = mode(q)
        w, h, at = q.screen_raw()
        if done(m, w, h) or time.time() >= end:
            return m, w, h, at
        time.sleep(1)


def aurora_now(q, timeout=15):
    """Polls until the background is painted for the size QEMU shows
    (aurora_covers), or the time is up: after a change of mode the
    first frame at the new size comes when the compositor gets to it"""
    end = time.time() + timeout
    while True:
        w, h, at = q.screen_raw()
        same, n = aurora_covers(at, w, h)
        if same == n or time.time() >= end:
            return same, n
        time.sleep(0.5)


def aurora_covers(at, w, h):
    """Where nothing stands - down the right-hand side, between the bar
    and the dock - the panel is the aurora painted at the panel's size"""
    spots = [(x, y) for x in range(w * 3 // 4, w, 23)
             for y in range(h // 8, h * 3 // 4, 23)]
    same = sum(1 for x, y in spots if at(x, y) == aurora_px(x, y, w, h))
    return same, len(spots)


def P(v, k):
    return v * k // 16


def ppx(px, k):
    return (px * k + 8) // 16


def native():
    """The screen says 1920x1080; then the setting says otherwise"""
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = boot(disk, slog, (1920, 1080), [])
        try:
            m = mode(q)
            check("the screen's own mode: the panel is 1920x1080 (%s)" % (m,),
                  m is not None and m[3:] == (1920, 1080))
            # QEMU's EDID says a hundred dots an inch, which is about
            # what the desktop was laid out for: no scale
            check("and at its density no scale is needed (%s)" % (m,),
                  m is not None and m[:3] == (1920, 1080, 100))
            w, h, at = q.screen_raw()
            check("QEMU shows a screen of that size (%dx%d)" % (w, h),
                  (w, h) == (1920, 1080))
            same, n = aurora_covers(at, w, h)
            check("and the aurora is painted at that size (%d/%d samples)"
                  % (same, n), same == n)

            # The setting, while the desktop runs: the mode changes
            # under it, and everything is painted for the new size
            check("the shell saves resolution = 1280x960",
                  set_as_admin(q, "resolution", "1280x960"))

            def at_1280(m, w, h):
                return (m is not None and m[3:] == (1280, 960) and
                        (w, h) == (1280, 960))
            m, w, h, at = settle(q, at_1280)
            check("'resolution 1280x960' changes the mode while the desktop "
                  "runs (%s, %dx%d)" % (m, w, h), at_1280(m, w, h))
            same, n = aurora_now(q)
            check("and the aurora is painted again for it (%d/%d samples)"
                  % (same, n), same == n)
        finally:
            q.close()

        q = boot(disk, slog, (1920, 1080), [], fresh=False)
        try:
            m = mode(q)
            w, h, at = q.screen_raw()
            check("kept: the next start is in that mode too, whatever the "
                  "screen says (%s, %dx%d)" % (m, w, h),
                  m is not None and m[3:] == (1280, 960) and
                  (w, h) == (1280, 960))
            check("the shell saves resolution = native",
                  set_as_admin(q, "resolution", "native"))

            def at_native(m, w, h):
                return (m is not None and m[3:] == (1920, 1080) and
                        (w, h) == (1920, 1080))
            m, w, h, at = settle(q, at_native)
            check("and 'native' gives the screen its own mode back (%s, "
                  "%dx%d)" % (m, w, h), at_native(m, w, h))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)


def calc_frame(lw, lh):
    cw, ch = define(C, "W0"), define(C, "H0")
    x, y = cascade(2, cw + 2, ch + TITLE_H + 2, w=lw, h=lh)
    return x, y, cw, ch


def one_and_a_half():
    """Nothing said about the screen; 1080 rows decide the scale"""
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = boot(disk, slog, None, [("resolution", "1920x1080")])
        try:
            m = mode(q)
            check("1080 rows and nothing said: one and a half, the desktop "
                  "1280x720 (%s)" % (m,),
                  m is not None and m == (1280, 720, 150, 1920, 1080))
            if not m:
                return
            k = 24
            lw, lh = 1280, 720
            w, h, at = q.screen_raw()
            same, n = aurora_covers(at, w, h)
            check("the aurora painted at the panel's size, not stretched "
                  "(%d/%d samples)" % (same, n), same == n)

            q.type_line("run gcalc")
            time.sleep(4)
            q.mouse_goto(900, 600)
            time.sleep(1)
            x, y, cw, ch = calc_frame(lw, lh)
            scr = q.screen_raw()
            got = q.find_ui(P(x + LIGHT_X + 2 * LIGHT_STEP + LIGHT_D + 6, k),
                            P(x + cw, k),
                            P(title_baseline(y), k), px=ppx(TITLE_PX, k),
                            face="bold", fg=DARK["title_text"], screen=scr)
            check("the title is FreeType's bold at one and a half times its "
                  "size, read pixel for pixel ('%s')" % got,
                  got == "Calculator")
            cx, cy = x + 1, y + CONTENT_DY

            def display():
                return q.find_ui(P(cx + 20, k), P(cx + cw - 10, k),
                                 P(cy + define(C, "DISP_BASE"), k),
                                 px=ppx(define(C, "DISP_PX"), k),
                                 fg=DARK["app_text"])
            got = display()
            check("and so is what the program draws: its display ('%s')"
                  % got, got == "0")

            # The pointer and the keys go by the layout
            def key(row, col):
                kx = define(C, "KEY_X0") + col * (define(C, "KEY_W") +
                                                  define(C, "KEY_GAP"))
                ky = define(C, "KEY_Y0") + row * (define(C, "KEY_H") +
                                                  define(C, "KEY_GAP"))
                q.click(cx + kx + define(C, "KEY_W") // 2,
                        cy + ky + define(C, "KEY_H") // 2)
                time.sleep(0.3)
            for rc in ((1, 0), (3, 3), (1, 1), (4, 3)):   # 7 + 8 =
                key(*rc)
            q.mouse_goto(900, 600)
            time.sleep(1)
            got = display()
            check("clicks land where the layout says: 7 + 8 = '%s'" % got,
                  got == "15")
            for ch_ in "c12*3\n":
                q.sendkey(Qemu.KEYNAMES.get(ch_, ch_))
                time.sleep(0.12)
            time.sleep(1)
            got = display()
            check("and keys reach it: 12 * 3 = '%s'" % got, got == "36")
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)


def twice():
    """Asked for two: the layout of 640x480 on a 1280x960 panel"""
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = boot(disk, slog, (1280, 960), [("scale", "200")])
        try:
            m = mode(q)
            check("asked for two: the desktop 640x480 on 1280x960 (%s)"
                  % (m,), m is not None and m == (640, 480, 200, 1280, 960))
            if not m:
                return
            k = 32
            q.launch_dock("gterm", w=m[0], h=m[1])    # (the layout's size)
            # The window opens over ten frames, and at this scale under
            # emulation that is seconds; until the last one it stands a
            # pixel inside its place. Read only once the title is where
            # the finished window has it.
            got = ""
            deadline = time.time() + 60
            while True:
                w, h, at = q.screen_raw()
                # The first application window's title bar, letters and
                # lights
                got = q.find_ui(P(94 + LIGHT_X + 2 * LIGHT_STEP + LIGHT_D + 6,
                                  k),
                                P(94 + TERM_W + 2, k),
                                P(title_baseline(54), k),
                                px=ppx(TITLE_PX, k), face="bold",
                                screen=(w, h, at))
                if got.startswith("Term") or time.time() > deadline:
                    break
                time.sleep(0.5)
            pairs = same = 0
            for yy in range(P(54, k), P(54 + TITLE_H, k), 2):
                for xx in range(P(94, k), P(94 + TERM_W + 2, k), 2):
                    pairs += 1
                    a = at(xx, yy)
                    if a == at(xx + 1, yy) == at(xx, yy + 1) == \
                       at(xx + 1, yy + 1):
                        same += 1
            check("not doubled: in a title bar the panel's pixels are not "
                  "squares of four (%d/%d are)" % (same, pairs),
                  pairs > 100 and same < pairs * 9 // 10)
            check("and its title reads at twice the size ('%s')" % got,
                  got.startswith("Term"))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)


A = "user/gappear.c"


def live():
    """At 100% on a 1920x1080 screen; then 150% picked in Appearance"""
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = boot(disk, slog, (1920, 1080), [])
        try:
            # ('runw': 'run' would wait for the program to end, and the
            # shell is asked things while both are open)
            q.type_line("runw gcalc")
            time.sleep(4)
            q.click(1900, 300)                   # the keys back to the shell
            time.sleep(0.5)
            q.type_line("runw gappear")
            time.sleep(4)
            q.click(1900, 300)
            time.sleep(0.5)
            # Nothing set yet: 'Auto', and QEMU's EDID says a hundred
            # dots an inch - the density the desktop was laid out for
            m = mode(q)
            check("before: 'Auto' gives 100%% on the 1920x1080 screen (%s)"
                  % (m,), m == (1920, 1080, 100, 1920, 1080))
            aw, ah = define(A, "W0"), define(A, "H0")
            ax, ay = cascade(3, aw + 2, ah + TITLE_H + 2, w=1920, h=1080)
            pad, sc_w = define(A, "PAD"), define(A, "SC_W")
            # The signed-in admin's Appearance writes the setting on the
            # click itself: it asks for a password only of a plain user
            # (user/gappear.c, apply())
            bx = ax + 1 + pad
            by = ay + CONTENT_DY + define(A, "SC_Y") + define(A, "SC_H") // 2
            _, _, at = q.screen_raw()
            lit = at(bx + 8, by)                 # 'Auto', the chip lit so far
            q.click(bx + 3 * (sc_w + 8) + sc_w // 2, by)
            # That chip going out - or the window drawn again at another
            # size - says Appearance has taken the click. Alt+F4 any
            # sooner goes to whatever had the focus before it. What the
            # click did is asked of the shell below, whichever way this
            # wait ends.
            end = time.time() + 20
            while time.time() < end:
                _, _, at = q.screen_raw()
                if at(bx + 8, by) != lit:
                    break
                time.sleep(0.3)
            q.sendkey("alt-f4")                  # Appearance, off the calculator
            time.sleep(2)
            q.click(1200, 300)                   # the shell, to ask it
            time.sleep(0.5)
            m, _, _, _ = settle(
                q, lambda m, w, h: m == (1280, 720, 150, 1920, 1080),
                timeout=10)
            check("'150%%' in Appearance: the desktop is 1280x720 while it "
                  "runs (%s)" % (m,), m == (1280, 720, 150, 1920, 1080))
            if m != (1280, 720, 150, 1920, 1080):
                return
            k = 24
            x, y, cw, ch = calc_frame(1280, 720)
            q.mouse_goto(900, 600)
            time.sleep(1)
            scr = q.screen_raw()
            got = q.find_ui(P(x + LIGHT_X + 2 * LIGHT_STEP + LIGHT_D + 6, k),
                            P(x + cw, k), P(title_baseline(y), k),
                            px=ppx(TITLE_PX, k), face="bold", screen=scr)
            check("the calculator that was open is there, its title at one "
                  "and a half ('%s')" % got, got == "Calculator")
            cx, cy = x + 1, y + CONTENT_DY

            def display():
                return q.find_ui(P(cx + 20, k), P(cx + cw - 10, k),
                                 P(cy + define(C, "DISP_BASE"), k),
                                 px=ppx(define(C, "DISP_PX"), k),
                                 fg=DARK["app_text"])
            got = display()
            check("and the program drew itself again at the new size ('%s')"
                  % got, got == "0")
            q.click(cx + cw // 2, cy + 40)       # its display: the focus
            time.sleep(0.5)
            for ch_ in "7*6\n":
                q.sendkey(Qemu.KEYNAMES.get(ch_, ch_))
                time.sleep(0.12)
            q.mouse_goto(900, 600)               # the pointer off the display
            time.sleep(1)
            got = display()
            check("it still counts: 7 * 6 = '%s'" % got, got == "42")

            q.click(1200, 300)
            time.sleep(0.5)
            check("the shell saves scale = 0",
                  set_as_admin(q, "scale", "0"))
            m, _, _, _ = settle(
                q, lambda m, w, h: m == (1920, 1080, 100, 1920, 1080))
            check("'Auto' again: the screen's own density, 100%% (%s)" % (m,),
                  m == (1920, 1080, 100, 1920, 1080))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)


def main():
    native()
    one_and_a_half()
    twice()
    live()
    return summary()


sys.exit(main())
