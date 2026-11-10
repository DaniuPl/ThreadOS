#!/usr/bin/env python3
"""ThreadOS smoke test: the programs in the dock come from a file.

The dock's programs were decided when the kernel was built. Adding one meant
editing kernel/desktop.c and building a kernel - which is the same as
saying that a person cannot add one.

The list is now a file on the disk, one line per icon:

    program  caption  picture-number

and nothing else. Plain text like everything else the system keeps: it
can be read with 'cat' and BUILT ON THE MACHINE ITSELF - here in the
text editor, three lines typed and saved as /desktop.icons -

    gterm Terminal 4
    gweb Internet 5
    gsettings Settings 6

because a format nobody on the machine can produce is a format for
somebody else's machine. The kernel's own 'cat' then reads back what
the editor wrote, so the file is checked by something that did not
write it.

The dock reads the list when the desktop starts, so the machine is
started again on the same disk. What is checked: the file is really
read (the kernel says so and the dock has as many programs as the file
has lines, with the names and pictures the file gave them), the tiles
still WORK (clicking one opens its program's window), and a machine
whose file says nothing usable keeps the programs it was born with - a
dock with nothing in it is a machine with nothing to click, and no
broken line should be able to do that.

A tile starts its program for whoever clicks it, an administrator
included, so the list is the system's own and three more things are
held: a line that names a path instead of a program is passed over
(the root is where any account may leave a file), a plain account can
neither replace the list nor create one where there is none, and a
list the disk says a plain account owns is not believed. The running
system no longer lets that last one come about, so the owner is
changed in the disk image between two starts - which is what a disk
from an older system, or from another machine, would look like.

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
from ptest import check, summary, _kernel_define

# The editor opens in slot 2 of the cascade, the first window there is
WX, WY = 94, 54
CX, CY = WX + 1, WY + CONTENT_DY


def builtin_labels():
    """The captions of the dock nobody configured, in order - read from
    kernel/desktop.c's icons[], as ptest reads the program names"""
    full = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                        "..", "kernel", "desktop.c")
    with open(full) as fh:
        m = re.search(r"icons\[ICON_MAX\] = \{(.*?)\n\};", fh.read(), re.S)
    return re.findall(r'\{"([^"]+)", (?:"[^"]+"|WEB_APP)', m.group(1))


def disk_owner(disk, name, uid=None):
    """Who the disk says owns the root's file 'name' - and, given a
    UID, makes that account its owner. Straight in the image, with no
    machine running: kernel/fs.c's inode is the name in FS_NAME_MAX + 1
    bytes, four of size, one that says the inode is in use, and one
    with the type in its low bit and the owner above it."""
    field = _kernel_define("kernel/include/fs.h", "FS_NAME_MAX") + 1
    shift = _kernel_define("kernel/fs.c", "FS_OWNER_SHIFT")
    key = name.encode().ljust(field, b"\0")
    with open(disk, "r+b") as fh:
        image = fh.read()
        hits = []
        at = image.find(key)
        while at >= 0:
            if image[at + field + 4] == 1:         # in use
                hits.append(at + field + 5)
            at = image.find(key, at + 1)
        if len(hits) != 1:
            return None
        kind = image[hits[0]]
        if uid is not None:
            fh.seek(hits[0])
            fh.write(bytes([(uid << shift) | (kind & ((1 << shift) - 1))]))
    return kind >> shift


def picture_of(at, g, k):
    """The inside of the k-th tile in the dock"""
    tx, ty = g["tiles"][k]
    t = g["tile"]
    return tuple(at(tx + i, ty + j) for j in range(3, t - 3)
                 for i in range(3, t - 3))


def wait_title(q, wx, wy, prefix, timeout=20):
    """The title of the window at (wx, wy) once it starts with 'prefix'
    - a program takes a while to start on a busy host"""
    got = ""
    end = time.time() + timeout
    while time.time() < end:
        got = q.window_title(wx, wy)
        if got.startswith(prefix):
            break
        time.sleep(0.5)
    return got


def sudo(q, command, want, timeout=30):
    """A command through 'sudo' in the kernel's shell, the password given
    when it is asked for. What arrived on the port, once 'want' has."""
    start = len(q.serial())
    q.type_line("sudo " + command)
    asked = False
    end = time.time() + timeout
    while time.time() < end:
        out = q.serial()[start:]
        if want in out:
            break
        if not asked and "password:" in out:
            q.type_line("thread")
            asked = True
        time.sleep(0.3)
    return q.serial()[start:]


def write_list(q, lines):
    """Builds the file in the text editor, the way a person would: a
    line typed, Enter, the next one - then Ctrl+S and the name."""
    q.launch_dock("gedit", n_w=0)          # the "Editor" tile
    got = wait_title(q, WX, WY, "Editor")
    check("the editor opens to write the list in ('%s')" % got,
          got.startswith("Editor"))
    q.mouse_goto(CX + 200, CY + 100)
    q.mouse_click()                        # the focus, into the page
    time.sleep(1)
    for i, line in enumerate(lines):
        if i:
            q.sendkey("ret")
        q.type_line("%s %s %d" % line, enter=False)
    time.sleep(1)
    q.sendkey("ctrl-s")
    time.sleep(1)
    q.type_line("/desktop.icons", enter=False)
    time.sleep(0.5)
    q.sendkey("ret")
    time.sleep(1)


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")
    wanted = [("gterm", "Terminal", 4),
              ("gweb", "Internet", 5),
              ("gsettings", "Settings", 6)]
    # A line that names a path and not a program, between the others:
    # it is in the file and must not be in the dock
    rooted = ("/gterm", "Rooted", 7)

    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()
            write_list(q, wanted[:1] + [rooted] + wanted[1:])

            # Read back by the kernel's shell, not by the editor
            q.click_desktop()
            shown = q.command("cat /desktop.icons", "gsettings Settings 6",
                              timeout=30)
            for app, label, art in wanted + [rooted]:
                check("the file has a line for '%s'" % app,
                      "%s %s %d" % (app, label, art) in shown)
        finally:
            q.close()

        # --- the next start reads it -----------------------------------
        print("--- the desktop started with the list on the disk ---")
        slog1 = tempfile.mktemp(suffix=".log")
        q = Qemu(KERNEL, INITRD, disk, serial=slog1)
        try:
            check("the desktop came up again", q.wait_desktop(timeout=40))
            q.mouse_home()
            q.mouse_goto(560, 300)
            time.sleep(2)

            log = q.serial()
            said = re.findall(r"icons from the file 'desktop.icons': \d+",
                              log)
            check("the kernel read the icon list and says how many: the "
                  "three programs, and not the path (%s)" % said,
                  "icons from the file 'desktop.icons': 3" in log)

            check("the desktop starts with no application windows",
                  q.dock_windows(n_l=3) == 0)

            for k, (app, label, art) in enumerate(wanted):
                got = q.dock_hint(k, n_l=3)
                check("tile %d carries the name from the file ('%s')"
                      % (k, got), got == label)
            q.mouse_goto(560, 300)
            time.sleep(1.5)

            g = dock_geom(n_l=3)
            w, h, at = q.screen_raw()
            pics = [picture_of(at, g, k) for k in range(3)]
            check("and each has a different picture",
                  pics[0] != pics[1] and pics[1] != pics[2] and
                  pics[0] != pics[2])

            # The tiles still do what they are for: the first one opens
            # a terminal, and its tile joins the dock
            q.click(*dock_tile(0, n_l=3))
            q.mouse_goto(560, 300)
            tx, ty = cascade(2, TERM_W + 2, TERM_H + TITLE_H + 2)
            got = wait_title(q, tx, ty, "Terminal")
            check("a click on a tile from the file starts its program "
                  "('%s', %d window in the dock)"
                  % (got, q.dock_windows(n_l=3)),
                  got.startswith("Terminal") and q.dock_windows(n_l=3) == 1)
        finally:
            q.close()
            if os.path.exists(slog1):
                os.unlink(slog1)

        # --- the list is an administrator's to write -------------------
        print("--- a plain account and the list ---")
        slog3 = tempfile.mktemp(suffix=".log")
        q3 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog3)
        uid = None
        try:
            q3.wait_for("thread64>", timeout=40)
            q3.command("login admin", "password:", timeout=20)
            out = q3.command("thread", "logged in as admin", timeout=30)
            check("the administrator signs in at the shell",
                  "logged in as admin" in out)
            q3.command("useradd dockuser", "admin password:", timeout=20)
            q3.command("thread", "new account password:", timeout=20)
            q3.command("dockpass", "again:", timeout=20)
            out = q3.command("dockpass", "account created", timeout=30)
            start = len(q3.serial())
            q3.type_line("users")
            m = None
            end = time.time() + 20
            while not m and time.time() < end:
                time.sleep(0.3)
                m = re.search(r"dockuser\s+user\s+uid (\d+)\r?\n",
                              q3.serial()[start:])
            uid = int(m.group(1)) if m else None
            check("a plain account is made (uid %s)" % uid,
                  "account created" in out and bool(uid))

            # With a list there a plain account could never write over
            # it; the way in was to make one where there was none. So
            # the administrator's goes first.
            out = q3.command("rm /desktop.icons", "rm:", timeout=20)
            gone = q3.command("cat /desktop.icons", "there is no file",
                              timeout=20)
            check("the administrator removes the list",
                  "'/desktop.icons' removed" in out and
                  "there is no file" in gone)

            q3.command("login dockuser", "password:", timeout=20)
            out = q3.command("dockpass", "logged in as dockuser", timeout=30)
            who = q3.command("whoami", "(", timeout=20)
            plain = "logged in as dockuser" in out and "dockuser (user)" in who
            check("the plain account signs in", plain)
            out = q3.command("write /desktop.icons gedit Planted 4",
                             "'/desktop.icons'", timeout=20)
            reply = [ln for ln in out.splitlines() if "'/desktop.icons'" in ln]
            back = q3.command("cat /desktop.icons", "cat:", timeout=20)
            check("and cannot make a list of its own (%s)" % reply,
                  plain and "written" not in out and
                  "Planted" not in back and "there is no file" in back)

            q3.command("login admin", "password:", timeout=20)
            out = q3.command("thread", "logged in as admin", timeout=30)
            said = q3.command("write /desktop.icons gterm Console 4",
                              "'/desktop.icons'", timeout=20)
            check("the administrator writes a list of one program",
                  "logged in as admin" in out and
                  "'/desktop.icons' written" in said)
        finally:
            q3.close()
            if os.path.exists(slog3):
                os.unlink(slog3)

        # The same list, but the disk now says the plain account owns it
        was = disk_owner(disk, "desktop.icons")
        now = disk_owner(disk, "desktop.icons", uid) if uid else None
        check("on the disk the list is the system's (uid %s); it is given "
              "to the plain account (uid %s)" % (was, uid),
              was == 0 and now == 0 and
              disk_owner(disk, "desktop.icons") == uid)

        print("--- a list a plain account owns ---")
        slog4 = tempfile.mktemp(suffix=".log")
        q4 = Qemu(KERNEL, INITRD, disk, serial=slog4)
        try:
            check("the desktop came up with it", q4.wait_desktop(timeout=40))
            q4.mouse_home()
            q4.mouse_goto(560, 300)
            want = "'desktop.icons' belongs to uid %s" % uid
            end = time.time() + 20
            out = q4.serial()
            while want not in out and time.time() < end:
                time.sleep(0.3)
                out = q4.serial()
            line = [ln for ln in out.splitlines() if "desktop.icons" in ln]
            check("the kernel says whose the list is and passes it over (%s)"
                  % line,
                  want in out and "passed over" in out and
                  "icons from the file" not in out)
            first = q4.dock_hint(0)
            check("the dock keeps its built-in programs: the first is the "
                  "usual one and not the list's ('%s')" % first,
                  first == builtin_labels()[0] and first != "Console")
        finally:
            q4.close()
            if os.path.exists(slog4):
                os.unlink(slog4)
        # Back to the system, so that what follows is about the lines in
        # the list and not about whose it is
        disk_owner(disk, "desktop.icons", 0)

        # --- a list that says nothing usable ---------------------------
        print("--- a file with no sensible lines ---")
        slog2 = tempfile.mktemp(suffix=".log")
        q2 = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE, serial=slog2)
        try:
            q2.wait_for("thread64>", timeout=40)
            # A file with nothing in it but a comment: the lines that
            # do not parse are passed over, and what is left is nothing.
            # The list belongs to the account that wrote it, and the
            # shell here has nobody signed in - so through 'sudo'.
            said = sudo(q2, "write /desktop.icons #only-a-comment",
                        "written")
            reply = [ln for ln in said.splitlines() if "write" in ln]
            check("the shell replaces the list with a comment (%s)" % reply,
                  "'/desktop.icons' written" in said)
            q2.type_line("desktop")
            check("the desktop came up after the comment",
                  q2.wait_desktop(timeout=40))
            q2.mouse_home()
            q2.mouse_goto(560, 300)
            time.sleep(2)

            # Said by the kernel when it read the file and found nothing
            # in it - which a file never read could not make it say
            end = time.time() + 20
            out = q2.serial()
            while ("nothing usable in 'desktop.icons'" not in out and
                   time.time() < end):
                time.sleep(0.3)
                out = q2.serial()
            check("the kernel read the file and found nothing usable in it",
                  "nothing usable in 'desktop.icons'" in out and
                  "icons from the file" not in out)
            labels = builtin_labels()
            check("the dock has no windows", q2.dock_windows() == 0)
            first = q2.dock_hint(0)
            check("the dock comes back to its built-in programs: the "
                  "first is the usual one ('%s')" % first,
                  first == labels[0])
            last = q2.dock_hint(DOCK_N - 1)
            check("and so is the last of all %d ('%s')" % (DOCK_N, last),
                  last == labels[-1])
        finally:
            q2.close()
            if os.path.exists(slog2):
                os.unlink(slog2)
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


sys.exit(main())
