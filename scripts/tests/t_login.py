#!/usr/bin/env python3
"""ThreadOS smoke test: first-run profile setup and graphical sign-in.

The first boot must create a profile before showing the desktop. The
profile must survive a restart, and a bad password must not unlock it.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary


def hint(q):
    w, h, at = q.screen_raw()
    fy = h * 2 // 3 - LOCK_FIELD_H
    under = lambda x, y: gfx_mix((0, 0, 0), aurora_px(x, y, w, h), 90)
    return q.find_ui(80, w - 80, fy + LOCK_FIELD_H + 22, px=UI_PX,
                     fg=(0xD8, 0xD8, 0xDC), under=under,
                     screen=(w, h, at))


def error_text(q):
    w, h, at = q.screen_raw()
    fy = h * 2 // 3 - LOCK_FIELD_H
    under = lambda x, y: gfx_mix((0, 0, 0), aurora_px(x, y, w, h), 90)
    return q.find_ui(80, w - 80, fy + LOCK_FIELD_H + 22, px=UI_PX,
                     fg=(0xFF, 0x69, 0x61), under=under,
                     screen=(w, h, at))


def wait_hint(q, text, timeout=20):
    deadline = time.time() + timeout
    while time.time() < deadline:
        got = hint(q)
        if text in got:
            return got
        time.sleep(0.25)
    return got


def setup_title(q):
    w, h, at = q.screen_raw()
    y = (h - 438) // 2
    return q.find_ui(80, w - 80, y + 34, px=20, face="bold",
                     screen=(w, h, at))


def wait_setup(q, timeout=30):
    deadline = time.time() + timeout
    got = ""
    while time.time() < deadline:
        try:
            got = setup_title(q)
            if "Welcome to ThreadOS" in got:
                return got
        except Exception:
            pass
        time.sleep(0.25)
    return got


def create_profile(q, login, display, password, light=False):
    q.type_line(login, enter=False)
    q.sendkey("tab")
    q.type_line(display, enter=False)
    q.sendkey("tab")
    if password:
        q.type_line(password, enter=False)
    q.sendkey("tab")
    if password:
        q.type_line(password, enter=False)
        q.sendkey("tab")
    if light:
        q.sendkey("l")
    q.sendkey("tab")
    q.sendkey("ret")


def wait_unlocked(q, timeout=30, dark=True):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            w, h, at = q.screen_raw()
            want = (aurora_px(600, 300, w, h, dark=dark)
                    if w >= 640 else q.DESK_BG)
            if w >= 640 and all(abs(at(600, 300)[i] - want[i]) <= 25
                                for i in range(3)):
                return True
        except Exception:
            pass
        time.sleep(0.3)
    return False


def main():
    disk = tempfile.mktemp(suffix=".img")
    log = tempfile.mktemp(suffix=".log")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    try:
        q = Qemu(KERNEL, INITRD, disk, serial=log)
        try:
            q.wait_serial("Starting the graphical desktop", timeout=45)
            got = wait_setup(q)
            check("a first-run profile wizard appears before the desktop",
                  "Welcome to ThreadOS" in got)

            create_profile(q, "admin", "Test Admin", "thread")
            check("the profile can be created with a separate login and display name",
                  wait_unlocked(q, timeout=30))

            q.sendkey("ctrl-alt-l")
            got = wait_hint(q, "Enter your password to unlock", timeout=15)
            check("the created profile can lock the desktop",
                  "Enter your password to unlock" in got)

            q.type_line("wrongpass")
            deadline = time.time() + 10
            got = ""
            while time.time() < deadline:
                got = error_text(q)
                if "Wrong password" in got:
                    break
                time.sleep(0.25)
            check("a wrong password is rejected on screen",
                  "Wrong password" in got)
            check("the desktop remains locked after a wrong password",
                  not wait_unlocked(q, timeout=2))

            q.type_line("thread")
            check("the admin password opens the desktop",
                  wait_unlocked(q, timeout=30))
        finally:
            q.close()

        passwordless_disk = tempfile.mktemp(suffix=".img")
        passwordless_log = tempfile.mktemp(suffix=".log")
        with open(passwordless_disk, "wb") as fh:
            fh.truncate(16 * 1024 * 1024)
        try:
            q = Qemu(KERNEL, INITRD, passwordless_disk,
                     serial=passwordless_log)
            try:
                q.wait_serial("Starting the graphical desktop", timeout=45)
                check("passwordless setup is offered on a fresh disk",
                      "Welcome to ThreadOS" in wait_setup(q))
                create_profile(q, "guest", "Guest Profile", "", light=True)
                check("an empty password signs in automatically with the selected theme",
                      wait_unlocked(q, timeout=30, dark=False))
            finally:
                q.close()

            q = Qemu(KERNEL, INITRD, passwordless_disk,
                     serial=passwordless_log)
            try:
                q.wait_serial("Starting the graphical desktop", timeout=45)
                check("a passwordless profile signs in automatically after restart",
                      wait_unlocked(q, timeout=30, dark=False))
            finally:
                q.close()
        finally:
            for path in (passwordless_disk, passwordless_log):
                if os.path.exists(path):
                    os.unlink(path)

        q = Qemu(KERNEL, INITRD, disk, serial=log)
        try:
            q.wait_serial("Starting the graphical desktop", timeout=45)
            got = wait_hint(q, "signs in", timeout=30)
            check("the saved profile replaces the first-run wizard after restart",
                  "signs in" in got and
                  "Welcome to ThreadOS" not in setup_title(q))
            q.type_line("thread")
            check("the saved profile password unlocks after restart",
                  wait_unlocked(q, timeout=30))
        finally:
            q.close()
    finally:
        for path in (disk, log):
            if os.path.exists(path):
                os.unlink(path)
    return summary()


sys.exit(main())
