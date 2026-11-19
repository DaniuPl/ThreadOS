#!/usr/bin/env python3
"""ThreadOS smoke test: how the lock opens - a PIN, a swipe, and when.

Settings' Screen Lock page chooses, for the signed-in account, what
opens its locked screen: its password, a PIN of 4 to 8 digits, or a
swipe up with nothing to type. Changing it takes the account's
password. Everything is done the way a person does it - in Settings,
with the mouse and the keys - and read off the lock screen:

  a PIN set in Settings opens the lock, and a wrong one does not; the
  password still opens it

  five wrong PINs and the PIN stops being taken until the password
  has been given - the lock says so

  a wrong password in Settings changes nothing

  with Swipe, a pull of a fifth of the screen upwards opens it, and a
  short one springs back

and 'lockafter': with it at 60 seconds the screen goes dark and comes
back without the lock; at 0 it comes back locked.

The PIN lives in the account file, which grew for it (version 3). A
disk from before - the account file rewritten on the image in the
version 2 layout, which an installed laptop has - still signs in with
its password, opens its lock with the password, and is written back
in version 3.

Run on its own or through scripts/run_tests.py.
"""

import os
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, _kernel_define
import ptest

_GS = "user/gsettings.c"
WAY_W = _kernel_define(_GS, "LOCK_WAY_W")
PIN_W = _kernel_define(_GS, "LOCK_PIN_W")
PW_W = _kernel_define(_GS, "LOCK_PW_W")
SEC_LOCK = GSET_SECTIONS.index("Screen Lock")


def typed(q, text):
    for ch in text:
        q.sendkey(Qemu.KEYNAMES.get(ch, ch))
        time.sleep(0.12)


def wrong_try(q, text):
    """A wrong try at the lock, and the wait for the desktop to have
    checked it - it says so on the serial port"""
    seen = q.serial().count("the lock stays shut")
    typed(q, text + "\n")
    end = time.time() + 20
    while time.time() < end and \
            q.serial().count("the lock stays shut") == seen:
        time.sleep(0.3)
    return q.serial().count("the lock stays shut") > seen


def lock_says(q):
    """The line under the lock's field, in its usual grey or in red"""
    w, h = q.size()
    fy = h * 2 // 3 - LOCK_FIELD_H
    under = lambda x, y: gfx_mix((0, 0, 0), aurora_px(x, y, w, h), 90)
    best = ""
    for fg in ((0xD8, 0xD8, 0xDC), (0xFF, 0x69, 0x61)):
        got = q.find_ui(80, w - 80, fy + LOCK_FIELD_H + 22, px=UI_PX,
                        fg=fg, under=under)
        if len(got) > len(best):
            best = got
    return best


def wait_says(q, want, timeout=8):
    got = ""
    end = time.time() + timeout
    while time.time() < end:
        got = lock_says(q)
        if want in got:
            break
        time.sleep(0.4)
    return got


def open_settings(q):
    """Settings, on its Screen Lock page: where its content starts"""
    W, H = q.size()
    q.launch_dock("gsettings")
    end = time.time() + 15
    while time.time() < end:
        for slot in range(8):
            x, y = cascade(slot, GSET_W + 2, GSET_H + TITLE_H + 2, w=W, h=H)
            if q.window_title(x, y, GSET_W + 2) == "Settings":
                cx, cy = x + 1, y + CONTENT_DY
                q.click(cx + GSET_SIDE_W // 2,
                        cy + GSET_SIDE_Y0 + SEC_LOCK * GSET_SIDE_H +
                        GSET_SIDE_H // 2)
                time.sleep(1)
                return cx, cy
        time.sleep(0.3)
    return None


def rows(cx, cy):
    """The page's rows, as draw_lockscreen() lays them out"""
    y_dim = GSET_PANE_Y0 + GSET_ROW_H - 2
    y_after = y_dim + GSET_CHIP_H + 10
    y_way = y_after + GSET_ROW_H - 2
    y_fields = y_way + GSET_ROW_H - 2
    way = lambda i: (cx + GSET_PANE_X + i * (WAY_W + GSET_CHIP_GAP) +
                     WAY_W // 2, cy + y_way + GSET_CTRL_DY + GSET_CHIP_H // 2)
    fy = cy + y_fields + GSET_CHIP_H // 2
    return dict(way=way, pin=(cx + GSET_PANE_X + PIN_W // 2, fy),
                pw=(cx + GSET_PANE_X + PIN_W + 8 + PW_W // 2, fy),
                note=(cx + GSET_PANE_X, cy + y_fields + GSET_CHIP_H + 6))


def settings_note(q, at):
    """The line under the fields: what the last Save came to"""
    x, y = rows(*at)["note"]
    return q.read_ui(x, ui_font_baseline_in(11, y, 16), 300, px=11,
                     fg=theme("dark")["app_dim"])


def choose(q, at, way, pin, password):
    """In Settings: a way to unlock, a PIN (or none), the password,
    Enter - and what Settings said to it, once it has said it (the
    password and the PIN are hashed a few thousand times, which takes
    an emulated machine seconds)"""
    r = rows(*at)
    q.click(*r["way"](way))
    time.sleep(0.4)
    if pin:
        q.click(*r["pin"])
        time.sleep(0.3)
        typed(q, pin)
    q.click(*r["pw"])
    time.sleep(0.3)
    typed(q, password + "\n")
    said = ""
    end = time.time() + 20
    while time.time() < end and not said.startswith(("Saved", "NOT")):
        time.sleep(0.5)
        said = settings_note(q, at)
    return said


def black(q):
    """Dark: black on the bare desktop, clear of the icons and the dock"""
    _, _, at = q.screen_raw()
    return all(max(at(x, y)) <= 4 for x, y in
               ((600, 300), (560, 200), (450, 350), (380, 160)))


def lock(q):
    q.mouse_goto(600, 300)
    q.sendkey("ctrl-alt-l")
    time.sleep(1.5)


USER_NAME_MAX = _kernel_define("user/userctl.h", "USER_NAME_MAX")
USER_DISPLAY_NAME_MAX = _kernel_define("user/userctl.h",
                                       "USER_DISPLAY_NAME_MAX")
USER_ACCOUNT_MAX = _kernel_define("user/userctl.h", "USER_ACCOUNT_MAX")

# kernel/user.c's layout: an account is its name, its display name,
# four bytes, a salt and a hash - and in version 3 the lock's way, a
# PIN flag, the PIN's salt and its hash after them
ACC_V2 = (USER_NAME_MAX + 1) + (USER_DISPLAY_NAME_MAX + 1) + 4 + 16 + 20
ACC_V3 = ACC_V2 + 1 + 1 + 16 + 20


def to_version2(disk):
    """The account file on the image, rewritten in version 2 by Linux's
    debugfs, the administrator's and private as the system keeps it.
    1 = done."""
    img = Ext2Image(disk)
    v3 = img.read("/.users")
    if not v3 or v3[:7] != b"THRUSR3" or \
            len(v3) != 12 + USER_ACCOUNT_MAX * ACC_V3:
        return 0
    v2 = b"THRUSR2\0" + v3[8:12] + b"".join(
        v3[12 + i * ACC_V3:12 + i * ACC_V3 + ACC_V2]
        for i in range(USER_ACCOUNT_MAX))
    return 1 if img.put("/.users", v2, 0o100600, 0, 0) else 0


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop())
            q.mouse_home()
            at = open_settings(q)
            check("Settings opens on Screen Lock", at is not None)
            if at is None:
                return summary()

            # A wrong password changes nothing
            said = choose(q, at, 1, "2468", "notmypassword")
            check("Settings says it did not change it ('%s')" % said,
                  said == "NOT changed (wrong password)")
            lock(q)
            said = wait_says(q, "to unlock")
            check("a wrong password in Settings sets no PIN: the lock asks "
                  "for the password ('%s')" % said,
                  said == "Enter your password to unlock")
            typed(q, "thread\n")
            time.sleep(2)

            # The PIN
            q.mouse_goto(600, 300)
            said = choose(q, at, 1, "2468", "thread")
            check("a PIN set in Settings ('%s')" % said,
                  said == "Saved - the lock opens the new way")
            lock(q)
            said = wait_says(q, "PIN")
            check("with a PIN chosen, the lock asks for it ('%s')" % said,
                  said == "Enter your PIN to unlock")
            wrong_try(q, "1357")
            said = wait_says(q, "Wrong")
            check("a wrong PIN opens nothing ('%s')" % said,
                  "Wrong" in said)
            typed(q, "2468\n")
            time.sleep(2)
            check("the right PIN opens it", lock_says(q) == "")

            lock(q)
            typed(q, "thread\n")
            time.sleep(2)
            check("and the password still does", lock_says(q) == "")

            # Five wrong PINs: the password only
            lock(q)
            got = [wrong_try(q, "0000") for _ in range(5)]
            check("five wrong PINs, each one checked (%s)" % got, all(got))
            said = wait_says(q, "Too many")
            check("after five wrong PINs the lock wants the password ('%s')"
                  % said, said == "Too many wrong PINs - enter your password")
            check("and the right PIN no longer opens it",
                  wrong_try(q, "2468") and lock_says(q) != "")
            typed(q, "thread\n")
            time.sleep(2)
            check("the password does", lock_says(q) == "")

            lock(q)
            said = wait_says(q, "PIN")
            check("which gives the PIN back ('%s')" % said,
                  said == "Enter your PIN to unlock")
            typed(q, "2468\n")
            time.sleep(2)
            check("and it opens the lock again", lock_says(q) == "")

            # Swipe
            q.mouse_goto(600, 300)
            said = choose(q, at, 2, "", "thread")
            check("Swipe chosen ('%s')" % said, said.startswith("Saved"))
            lock(q)
            said = wait_says(q, "Swipe")
            check("with Swipe the lock asks for nothing but a pull ('%s')"
                  % said, said == "Swipe up to unlock")
            w, h = q.size()
            q.mouse_goto(w // 2, h * 3 // 4)
            time.sleep(0.4)
            q.mouse_button(True)
            q.mouse_goto(w // 2, h * 3 // 4 - h // 12)
            time.sleep(0.4)
            q.mouse_button(False)
            time.sleep(1.5)
            check("a short pull springs back", "Swipe" in lock_says(q))
            q.mouse_goto(w // 2, h * 3 // 4)
            time.sleep(0.4)
            q.mouse_button(True)
            for k in range(1, 5):
                q.mouse_goto(w // 2, h * 3 // 4 - k * h // 12)
                time.sleep(0.2)
            q.mouse_button(False)
            time.sleep(1.5)
            check("a pull of a third of the screen opens it",
                  lock_says(q) == "")

            # Back to the password, and when the lock comes
            q.mouse_goto(600, 300)
            said = choose(q, at, 0, "", "thread")
            check("and the password again ('%s')" % said,
                  said.startswith("Saved"))
            q.click_desktop()
            q.sudo("set screenoff 8", "saved")
            q.sudo("set lockafter 60", "saved")
            q.mouse_goto(630, 250)
            end = time.time() + 20
            while time.time() < end and not black(q):
                time.sleep(0.5)
            check("the screen goes dark", black(q))
            q.mouse_move(3, 0)
            q.mx += 3
            time.sleep(2)
            check("and within lockafter it comes back unlocked",
                  lock_says(q) == "" and not black(q))
            q.click_desktop()
            q.sudo("set lockafter 0", "saved")
            q.mouse_goto(630, 250)
            end = time.time() + 20
            while time.time() < end and not black(q):
                time.sleep(0.5)
            q.mouse_move(3, 0)
            q.mx += 3
            said = wait_says(q, "to unlock")
            check("with lockafter 0 it comes back locked ('%s')" % said,
                  said == "Enter your password to unlock")
            typed(q, "thread\n")
            time.sleep(2)
        finally:
            q.close()

        # A disk from before the PIN
        print("--- an account file of version 2 ---")
        check("the account file rewritten in the version 2 layout",
              to_version2(disk))
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop comes up and the account signs in with its "
                  "password", q.wait_desktop())
            q.mouse_home()
            lock(q)
            said = wait_says(q, "to unlock")
            check("its lock asks for the password, as it did ('%s')" % said,
                  said == "Enter your password to unlock")
            typed(q, "thread\n")
            time.sleep(2)
            check("which opens it", lock_says(q) == "")
        finally:
            q.close()
        after = Ext2Image(disk).read("/.users") or b""
        check("and the file is version 3 again (%s, %d bytes)"
              % (after[:7], len(after)),
              after[:7] == b"THRUSR3" and
              len(after) == 12 + USER_ACCOUNT_MAX * ACC_V3)
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


sys.exit(main())
