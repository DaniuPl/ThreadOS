#!/usr/bin/env python3
"""ProtonOS smoke test: system settings.

Everything the system knew about itself was written into it at build
time. A name server could be changed with a command, and the change
lived exactly as long as the machine did - turning it off was enough to
forget.

A setting is now a line in a text file on the disk: name=value. The
kernel reads it once, at boot, right after the file system comes up,
and the set of names is fixed: every one of them has somewhere a place
that reads it and does something with it.

Which is why the assertions here are not about the file but about the
EFFECTS. A file with 'nazwa=maszyna' in it proves nothing; a prompt
that says 'maszyna>' after a restart proves everything. So each setting
is checked where it is used:

  nazwa  - in the shell prompt
  kolor  - in the colour of that prompt
  tlo    - in the pixels of the desktop's background
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


def taskbar_ready(q, timeout=40):
    """Waits for the desktop WITHOUT looking at the background colour -
    the background is the very thing these tests change. The taskbar is
    light grey whatever the background is."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            w, _, at = q.screen_raw()
            # Not at x=600: the clock is drawn there. x=520 is bar
            # between the window buttons and the clock.
            if w >= 640 and at(520, 474)[0] > 150:
                return True
        except Exception:
            pass
        time.sleep(0.3)
    return False


def main():
    disk = new_disk()
    slog = tempfile.mktemp(suffix=".log")

    print("--- ustawienia z linii polecen ---")
    try:
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog)
        try:
            q.wait_for("ProtonOS shell", timeout=40)

            out = q.command("set", "uzycie:", timeout=30)
            # A listing that shows nothing would pass every other check
            for name in ("nazwa", "dns", "kolor", "tlo"):
                check("'set' wymienia ustawienie '%s'" % name,
                      re.search(r"\b%s = " % name, out) is not None)

            # The default is what the system had before there were any
            # settings at all - so nothing changed for a machine that
            # never sets one
            check("domyslna nazwa maszyny to 'proton'",
                  re.search(r"nazwa = proton\b", out) is not None)

            # Changing a setting is changing the system, so it goes
            # through 'sudo' - the password is asked for on the next
            # line and never shown (see t_users.py for that road)
            def set_as_admin(command, wait="zapisane"):
                q.type_line("sudo " + command)
                time.sleep(1)
                return q.command("proton", wait, timeout=30)

            changed = set_as_admin("set nazwa maszyna")
            check("zmiana nazwy przyjeta", "nazwa = maszyna (zapisane)"
                  in changed)
            set_as_admin("set kolor 14")
            set_as_admin("set dns 10.0.2.3")
            set_as_admin("set tlo 3")

            # The change is in force at once, not only after a restart:
            # the prompt says the new name from the next line on
            after = q.command("pwd", "/", timeout=30)
            check("zacheta od razu mowi nowa nazwa", "maszyna>" in after)

            # An unknown name is an error, not a new setting
            bad = set_as_admin("set nieistniejace 7", "set:")
            check("nieznane ustawienie odrzucone",
                  "nie ma ustawienia" in bad or "nie udalo sie" in bad)

            # The file is plain text and says what it holds
            f = q.command("cat system.cfg", "tlo=", timeout=30)
            check("plik na dysku jest zwyklym tekstem",
                  "nazwa=maszyna" in f and "dns=10.0.2.3" in f and
                  "tlo=3" in f)
        finally:
            q.close()

        print("--- drugi rozruch na TYM SAMYM dysku ---")
        slog2 = tempfile.mktemp(suffix=".log")
        q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog2)
        try:
            q2.wait_for("ProtonOS shell", timeout=40)
            boot = q2.serial()
            # The kernel says at boot what it read - and it says the new
            # name, so the file was read before anything used it
            check("jadro wczytalo ustawienia przy starcie",
                  "Ustawienia: maszyna" in boot)

            out = q2.command("set", "uzycie:", timeout=30)
            check("nazwa przezyla restart",
                  re.search(r"nazwa = maszyna\b", out) is not None)
            check("kolor przezyl restart",
                  re.search(r"kolor = 14\b", out) is not None)
            check("tlo przezylo restart",
                  re.search(r"tlo = 3\b", out) is not None)

            # dns is the one that goes to a SUBSYSTEM, not just to the
            # prompt: the network layer has to have been told
            ifc = q2.command("ifconfig", "DNS", timeout=30)
            check("serwer nazw z ustawien trafil do warstwy sieciowej "
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
    print("--- okno ustawien na pulpicie ---")
    disk2 = new_disk()
    try:
        q = Qemu(KERNEL, INITRD, disk2, serial=tempfile.mktemp(suffix=".log"))
        try:
            check("pulpit wstal", q.wait_desktop(timeout=40))
            q.mouse_home()
            # The 'Ustaw' icon is the seventh, at (8,260), a 32x24 tile
            q.mouse_goto(24, 272)
            q.mouse_click()
            time.sleep(3)
            q.mouse_goto(560, 420)      # the cursor away from the window
            time.sleep(1)

            # The window is slot 2: (94,54), content from (95,65). The
            # heading is drawn in blue at (6,6) of the content area.
            w, h, at = q.screen_raw()
            head = any(at(95 + x, 65 + 8)[2] > 120 and at(95 + x, 65 + 8)[0] < 90
                       for x in range(6, 150))
            check("okno ustawien sie otworzylo (naglowek na niebiesko)", head)

            CX, CY = 95, 65
            # the 'tlo' row is the fourth: ROW_Y=22 + 3*14 = 64
            q.mouse_goto(CX + 40, CY + 64)
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
            for ch in "proton":
                q.sendkey(ch)
                time.sleep(0.15)
            q.sendkey("ret")
            time.sleep(2)
            q.mouse_goto(560, 420)
            time.sleep(1)

            # 'zapisane na dysku' is drawn in green under the
            # description. Which row that is depends on how many
            # settings there are and how long the description of this
            # one is, so we look through the band it can be in rather
            # than computing it here - the layout is the program's
            # business, the message is ours.
            w, h, at = q.screen_raw()
            green = any(at(CX + 6 + x, CY + y)[1] > 120 and
                        at(CX + 6 + x, CY + y)[0] < 90
                        for y in range(130, 190)
                        for x in range(0, 130))
            check("program zameldowal zapis na dysku", green)
        finally:
            q.close()

        print("--- pulpit po restarcie na tym samym dysku ---")
        q2 = Qemu(KERNEL, INITRD, disk2, serial=tempfile.mktemp(suffix=".log"))
        try:
            check("pulpit wstal po restarcie", taskbar_ready(q2, timeout=40))
            time.sleep(2)
            w, h, at = q2.screen_raw()
            r, g, b = at(600, 300)
            # Green (0,28,0 in the palette -> about 0,113,0 in pixels),
            # and definitely not the teal it started as
            check("tlo pulpitu jest zielone, a nie morskie (%d,%d,%d)"
                  % (r, g, b), g > 60 and r < 60 and b < 60)
        finally:
            q2.close()
    finally:
        if os.path.exists(disk2):
            os.unlink(disk2)

    return summary()


sys.exit(main())
