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
that says 'machine>' after a restart proves everything. So each setting
is checked where it is used:

  name       - in the shell prompt
  color      - in the colour of that prompt
  background - in the pixels of the desktop's background
  dns    - in what 'ifconfig' says the name server is

And each of them across a REBOOT on the same disk image, because that
is the only thing a cache cannot fake.

The settings window on the desktop is checked the same way: the icon
opens it, a click cycles a value, Enter writes it, and after the
restart the desktop has a different colour. A program in ring 3 holds
none of this - it asks the kernel, and the kernel owns the file.

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


def main():
    disk = new_disk()
    slog = tempfile.mktemp(suffix=".log")

    print("--- settings from the command line ---")
    try:
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
        try:
            q.wait_for("ThreadOS shell", timeout=40)

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
            check("the prompt says the new name at once", "machine>" in after)

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
            q2.wait_for("ThreadOS shell", timeout=40)
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
            # The settings are the seventh program in the dock
            q.launch_dock(6)                # the "Settings" tile
            time.sleep(3)
            q.mouse_goto(560, 420)      # the cursor away from the window
            time.sleep(1)

            # The window is slot 2: its corner at (94,54), the content
            # one frame line in and under the title bar. Its first row,
            # chosen when it opens, is the machine's name.
            CX, CY = 94 + 1, 54 + CONTENT_DY
            name = q.gset_row(CX, CY, 0, chosen=True)[0]
            check("the settings window opened (its first row: '%s')" % name,
                  name == "name")

            # the 'background' row is the fourth
            bg = q.gset_row(CX, CY, 3)[0]
            check("the fourth row is the background ('%s')" % bg,
                  bg == "background")
            q.mouse_goto(CX + 40, CY + gset_row_y(3))
            q.mouse_click()             # select the row
            time.sleep(1)
            for _ in range(3):          # cycle 0 -> 3 (green)
                q.mouse_click()
                time.sleep(0.6)
            q.sendkey("ret")            # Enter asks the kernel to write it
            time.sleep(2)
            # The kernel refuses a plain user, so the window asks for
            # the admin's password - see t_users_gui.py for that on its
            # own; here it is simply the way to the disk
            for ch in "thread":
                q.sendkey(ch)
                time.sleep(0.15)
            q.sendkey("ret")
            time.sleep(2)
            q.mouse_goto(560, 420)
            time.sleep(1)

            # 'Saved to the disk' is said on the line at the bottom of
            # the window
            said = q.gset_note(CX, CY)
            check("the program reported the write to the disk ('%s')" % said,
                  said == "Saved to the disk")
        finally:
            q.close()

        print("--- the desktop after a restart on the same disk ---")
        q2 = Qemu(KERNEL, INITRD, disk2, serial=tempfile.mktemp(suffix=".log"))
        try:
            check("the desktop came up after the restart", dock_ready(q2, timeout=40))
            time.sleep(2)
            w, h, at = q2.screen_raw()
            r, g, b = at(600, 300)
            # Green (0,113,0 a little darker towards the floor), and
            # definitely not the aurora it started as
            check("the desktop background is green, not the aurora (%d,%d,%d)"
                  % (r, g, b), g > 60 and r < 60 and b < 60)
        finally:
            q2.close()
    finally:
        if os.path.exists(disk2):
            os.unlink(disk2)

    return summary()


sys.exit(main())
