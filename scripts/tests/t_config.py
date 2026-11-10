#!/usr/bin/env python3
"""ThreadOS smoke test: system settings.

Everything the system knew about itself was written into it at build
time. A name server could be changed with a command, and the change
lived exactly as long as the machine did - turning it off was enough to
forget.

A setting is now a line in a text file on the disk: name=value. The
kernel reads it once, at boot, right after the file system comes up,
and the set of names is fixed: every one of them has somewhere a place
that reads it and does something with it.

Which is why the assertions here are not about the file but about the
EFFECTS. A file with 'name=machine' in it proves nothing; a prompt
that says 'machine64>' after a restart proves everything. So each setting
is checked where it is used:

  name       - in the shell prompt
  color      - in the colour of that prompt, read off the console the
               kernel draws on the framebuffer
  background - in the pixels of the desktop's background
  dns    - in what 'ifconfig' says the name server is

And each of them across a REBOOT on the same disk image, because that
is the only thing a cache cannot fake.

The settings window on the desktop is checked the same way: its tile in
the dock opens it, a click on a colour under Appearance writes it at
once, and after the restart the desktop has a different colour. The
window runs in the signed-in admin's session, so the kernel takes the
change without asking for a password (t_usersgui.py checks that on its
own). A program in ring 3 holds none of this - it asks the kernel, and
the kernel owns the file.

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

DISK_MB = 16

# 'set color 14': yellow in the sixteen colours of VGA text mode, which
# the framebuffer console draws its letters in (kernel/fbcon.c)
YELLOW = (0xFF, 0xFF, 0x55)
# And what the prompt wears when nobody has set one: light green, the
# default in prompt() (kernel64/shell64.c)
GREEN = (0x55, 0xFF, 0x55)


def prompt_colour(q, prompt, timeout=10, want=None):
    """The colour of the last line on the console that holds 'prompt'.
    Without a desktop the kernel draws its console on GRUB's framebuffer:
    80x25 cells of 8x8 in the middle of the screen, every letter on
    black - so the letters' colour is that of the pixels of their cells
    that are not black. Read until it is 'want', if one is given, or the
    time is up; None when there is no such line."""
    got = None
    end = time.time() + timeout
    while time.time() < end:
        # The text and the colours from two screenshots, and a third to
        # make sure nothing scrolled between them
        w, h, _ = q.screen_raw()
        x0, y0 = (w - 80 * 8) // 2, (h - 25 * 8) // 2
        text = q.read_text(x0, y0, 80, 25)
        _, _, at = q.screen_raw()
        if q.read_text(x0, y0, 80, 25) == text:
            got = None
            lines = text.split("\n")
            for row in range(len(lines) - 1, -1, -1):
                col = lines[row].find(prompt)
                if col < 0:
                    continue
                seen = {}
                for dy in range(8):
                    for dx in range(len(prompt) * 8):
                        p = at(x0 + col * 8 + dx, y0 + row * 8 + dy)
                        if p != (0, 0, 0):
                            seen[p] = seen.get(p, 0) + 1
                if seen:
                    got = max(seen, key=seen.get)
                break
            if want is None or got == want:
                return got
        time.sleep(0.5)
    return got


def new_disk():
    path = tempfile.mktemp(suffix=".img")
    with open(path, "wb") as f:
        f.truncate(DISK_MB * 1024 * 1024)
    return path


def dock_ready(q, timeout=40):
    """Waits for the desktop WITHOUT looking at the background colour -
    the background is the very thing these tests change. The dock's
    first tile has its white window on it whatever the background is."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            w, _, at = q.screen_raw()
            if w >= 640 and min(at(*dock_tile(0))) > 200:
                return True
        except Exception:
            pass
        time.sleep(0.3)
    return False


def sign_in(q, timeout=60):
    """A restarted desktop opens on its lock: the first start made a
    profile (Qemu.wait_desktop, password 'thread'). Qemu.wait_desktop
    reads the lock's hint against the aurora, and the background here is
    the green the settings chose - so the hint is read against whatever
    lies under it, and the password typed once it asks for it."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            w, h, at = q.screen_raw()
            if w >= 640:
                fy = h * 2 // 3 - LOCK_FIELD_H
                hint = q.find_ui(80, w - 80, fy + LOCK_FIELD_H + 22,
                                 px=UI_PX, fg=(0xD8, 0xD8, 0xDC),
                                 screen=(w, h, at))
                if "signs in" in hint:
                    q.type_line("thread")
                    return True
        except Exception:
            pass
        time.sleep(0.5)
    return False


def main():
    disk = new_disk()
    slog = tempfile.mktemp(suffix=".log")

    print("--- settings from the command line ---")
    try:
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
        try:
            q.wait_for("thread64>", timeout=40)

            out = q.command("set", "usage:", timeout=30)
            # A listing that shows nothing would pass every other check
            for name in ("name", "dns", "color", "background"):
                check("'set' lists the setting '%s'" % name,
                      re.search(r"\b%s = " % name, out) is not None)

            # The default is what the system had before there were any
            # settings at all - so nothing changed for a machine that
            # never sets one
            check("the machine's default name is 'thread'",
                  re.search(r"name = thread\b", out) is not None)
            # What the prompt wears before anybody sets a colour - what
            # makes the yellow below the setting's doing
            before = prompt_colour(q, "thread64", want=GREEN)
            check("the prompt starts in the default colour, light green "
                  "(%s)" % (before,), before == GREEN)

            # Changing a setting is changing the system, so it goes
            # through 'sudo' - the password is asked for on the next
            # line and never shown (see t_users.py for that road)
            def set_as_admin(command, wait="saved"):
                q.type_line("sudo " + command)
                time.sleep(1)
                return q.command("thread", wait, timeout=30)

            changed = set_as_admin("set name machine")
            check("the name change was accepted", "name = machine (saved)"
                  in changed)
            set_as_admin("set color 14")
            set_as_admin("set dns 10.0.2.3")
            set_as_admin("set background 3")

            # The change is in force at once, not only after a restart:
            # the prompt says the new name from the next line on
            after = q.command("pwd", "/", timeout=30)
            check("the prompt says the new name at once", "machine64>" in after)
            now = prompt_colour(q, "machine64", want=YELLOW)
            check("and wears the new colour at once (%s, %s before)"
                  % (now, before), now == YELLOW)

            # An unknown name is an error, not a new setting
            bad = set_as_admin("set nosuchsetting 7", "set:")
            check("an unknown setting is refused",
                  "there is no setting" in bad or "could not" in bad)

            # The file is plain text and says what it holds
            f = q.command("cat system.cfg", "background=", timeout=30)
            check("the file on the disk is plain text",
                  "name=machine" in f and "dns=10.0.2.3" in f and
                  "background=3" in f)
        finally:
            q.close()

        print("--- a second boot on the SAME disk ---")
        slog2 = tempfile.mktemp(suffix=".log")
        q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog2)
        try:
            # (the shell's greeting: the prompt has another name now)
            q2.wait_for("Commands:", timeout=40)
            boot = q2.serial()
            # The kernel says at boot what it read - and it says the new
            # name, so the file was read before anything used it
            check("the kernel read the settings at boot",
                  "Settings: machine" in boot)

            out = q2.command("set", "usage:", timeout=30)
            check("the name survived the restart",
                  re.search(r"name = machine\b", out) is not None)
            check("the colour survived the restart",
                  re.search(r"color = 14\b", out) is not None)
            now = prompt_colour(q2, "machine64", want=YELLOW)
            check("and the prompt wears it after the restart (%s)" % (now,),
                  now == YELLOW)
            check("the background survived the restart",
                  re.search(r"background = 3\b", out) is not None)

            # dns is the one that goes to a SUBSYSTEM, not just to the
            # prompt: the network layer has to have been told
            ifc = q2.command("ifconfig", "DNS", timeout=30)
            check("the name server from the settings reached the network layer "
                  "(ifconfig)", "10.0.2.3" in ifc)
        finally:
            q2.close()
            if os.path.exists(slog2):
                os.unlink(slog2)
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    # --- The same thing through the window on the desktop -------------
    print("--- the settings window on the desktop ---")
    disk2 = new_disk()
    try:
        q = Qemu(KERNEL, INITRD, disk2, serial=tempfile.mktemp(suffix=".log"))
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()
            q.launch_dock("gsettings")                # the "Settings" tile
            q.mouse_goto(560, 420)      # the cursor away from the window

            def read_until(read, want, timeout=20):
                got = None
                end = time.time() + timeout
                while time.time() < end:
                    got = read()
                    if got == want:
                        break
                    time.sleep(0.3)
                return got

            # The window is slot 2 of the cascade, its content one frame
            # line in and under the title bar. It opens on General.
            wx, wy = cascade(2, GSET_W + 2, GSET_H + TITLE_H + 2)
            CX, CY = wx + 1, wy + CONTENT_DY
            head = read_until(lambda: q.gset_heading(CX, CY), "General")
            check("the settings window opened on General ('%s')" % head,
                  head == "General")

            # The background is under Appearance: green is the fourth
            q.click(CX + gset_section("Appearance")[0],
                    CY + gset_section("Appearance")[1])
            q.mouse_goto(560, 420)
            head = read_until(lambda: q.gset_heading(CX, CY), "Appearance")
            check("a click on Appearance opens it ('%s')" % head,
                  head == "Appearance")
            # The window was started from the dock of the admin signed in
            # by wait_desktop, so it runs as the admin: the kernel takes
            # the setting on the click alone, with no password asked
            green = gset_swatch(3)
            q.click(CX + green[0], CY + green[1])
            q.mouse_goto(560, 420)

            # 'Saved' is said on the line at the bottom of the window
            said = read_until(lambda: q.gset_status(CX, CY), "Saved")
            check("the program reported the write to the disk ('%s')" % said,
                  said == "Saved")
        finally:
            q.close()

        print("--- the desktop after a restart on the same disk ---")
        q2 = Qemu(KERNEL, INITRD, disk2, serial=tempfile.mktemp(suffix=".log"))
        try:
            check("the restarted desktop asks for the profile's password",
                  sign_in(q2))
            check("the desktop came up after the restart", dock_ready(q2, timeout=40))
            # Green (0,113,0 a little darker towards the floor), and
            # definitely not the aurora it started as
            end = time.time() + 15
            while True:
                w, h, at = q2.screen_raw()
                r, g, b = at(600, 300)
                if (g > 60 and r < 60 and b < 60) or time.time() > end:
                    break
                time.sleep(0.3)
            check("the desktop background is green, not the aurora (%d,%d,%d)"
                  % (r, g, b), g > 60 and r < 60 and b < 60)
        finally:
            q2.close()
    finally:
        if os.path.exists(disk2):
            os.unlink(disk2)

    return summary()


sys.exit(main())
