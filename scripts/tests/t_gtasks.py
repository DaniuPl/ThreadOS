#!/usr/bin/env python3
"""ThreadOS smoke test: the task manager in a window.

The shell has printed 'ps' since the beginning, but printing is not
something a program can read, and a list one has to type a command for
is a list nobody looks at. A machine with a dozen windows on it needs
one place that says what is running - and, when a program stops
answering, a way to end it that is not "restart the machine".

So there is a row per task, refreshed twice a second, a click to
choose one and a button to end it. Two things it will not do: end a
kernel thread (those share the kernel's memory, and ending one takes a
piece of the machine with it) and end itself (a task manager that
vanishes when clicked is a joke rather than a tool).

The assertions are about all of that, and the one that matters most is
that the list is LIVE: a program started while the window is open
appears in it by itself, and one ended through the button leaves both
the list and the taskbar.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, font8x8

TB_H = 18
TBL_X0 = 46
CLOCK_W = 8 * 8 + 6

# The window: slot 2 of the cascade, its content a pixel right and a
# title bar down. gtasks asks for 300x190.
WX, WY = 94, 54
CX, CY = WX + 1, WY + CONTENT_DY
ROW_Y0 = 26
ROW_H = 10


def is_bar(px):
    return all(abs(c - 210) <= 6 for c in px)


def is_grey(px):
    return all(abs(c - 227) <= 8 for c in px)


def field(at, x, y, cols):
    """A piece of text in the window: anything that is not the window's
    own grey is ink. The chosen row has a blue background, so what is
    read is 'not the background', not 'dark'."""
    font = font8x8()
    x0, y0 = CX + x, CY + y
    out = ""
    for c in range(cols):
        pattern = []
        for yy in range(8):
            bits = 0
            for xx in range(8):
                p = at(x0 + c * 8 + xx, y0 + yy)
                if not is_grey(p) and not (p[2] > 120 and p[0] < 90
                                           and p[1] < 90):
                    bits |= 1 << xx
            pattern.append(bits)
        out += font.get(tuple(pattern), " " if not any(pattern) else "?")
    return out.strip()


def rows(at, n=12):
    """The list as it stands: (pid, name, state, kind) per row."""
    out = []
    for i in range(n):
        y = ROW_Y0 + i * ROW_H
        name = field(at, 44, y, 12)
        if not name:
            break
        out.append((field(at, 4, y, 4), name, field(at, 160, y, 7),
                    field(at, 232, y, 8)))
    return out


def picked_row(at, n=12):
    """Which row wears the accent colour - the one that was chosen."""
    for i in range(n):
        y = CY + ROW_Y0 + i * ROW_H
        p = at(CX + 130, y + 3)
        if p[2] > 120 and p[0] < 90 and p[1] < 90:
            return i
    return -1


def buttons(at, w, y):
    out, start = [], None
    for x in range(TBL_X0, w - CLOCK_W - 2):
        on_bar = is_bar(at(x, y))
        if not on_bar and start is None:
            start = x
        elif on_bar and start is not None:
            out.append((start, x - 1))
            start = None
    if start is not None:
        out.append((start, w - CLOCK_W - 3))
    return out


def sixty_four():
    """The same window on the 64-bit kernel. The program is one source
    compiled twice; what is asked here is the part that is not - the
    call that hands over one row of the task list, which long mode
    answers with its own code."""
    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("SKIPPED (64): no 64-bit image (make kernel64)")
        return
    if shutil.which("grub-mkrescue") is None or \
       shutil.which("qemu-system-x86_64") is None:
        print("SKIPPED (64): no grub-mkrescue or qemu-system-x86_64")
        return

    global CX, CY
    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)

    try:
        os.makedirs(d + "/boot/grub")
        shutil.copy(KERNEL64, d + "/boot/threados64.bin")
        mod = ""
        if INITRD64 and os.path.exists(INITRD64):
            shutil.copy(INITRD64, d + "/boot/initrd64.img")
            mod = "    module2 /boot/initrd64.img initrd64\n"
        with open(d + "/boot/grub/grub.cfg", "w") as fh:
            fh.write("insmod all_video\nset gfxpayload=keep\n"
                     "set timeout=0\nset default=0\n"
                     'menuentry "ThreadOS 64" {\n'
                     "    multiboot2 /boot/threados64.bin\n" + mod + "}\n")
        made = subprocess.run(["grub-mkrescue", "-o", iso, d],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
        check("the disc with the 64-bit kernel builds", made)
        if not made:
            return

        q = Qemu(KERNEL, INITRD, disk=disk, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=60)
            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=30)
            time.sleep(4)
            q.mouse_home()

            q.mouse_goto(24, 352)              # the "Tasks" icon
            q.mouse_click()
            time.sleep(5)
            q.mouse_goto(WX + 100, WY + 4)     # out of the cascade
            time.sleep(0.4)
            q.mouse_button(True)
            time.sleep(0.4)
            q.mouse_goto(400, 240)
            time.sleep(0.5)
            q.mouse_goto(400, 240)
            time.sleep(0.5)
            q.mouse_button(False)
            time.sleep(1.5)
            CX, CY = 300 + 1, 236 + CONTENT_DY
            q.mouse_goto(600, 430)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            row = h - TB_H + 2
            listed = rows(at)
            names = [r[1] for r in listed]
            check("(64) the task list is there (%d rows: %s)"
                  % (len(listed), ", ".join(names)), len(listed) >= 3)
            check("(64) it sees itself", any("gtasks" in n for n in names))
            check("(64) and it tells a program from a kernel thread",
                  any(r[3] == "program" for r in listed) and
                  any(r[3] == "kernel" for r in listed))

            q.mouse_goto(24, 72)               # the "Clock" icon
            q.mouse_click()
            time.sleep(5)
            q.mouse_goto(600, 430)
            time.sleep(2)
            w, h, at = q.screen_raw()
            listed = rows(at)
            clock_row = [i for i, r in enumerate(listed) if "gclock" in r[1]]
            check("(64) a new program appears on the list by itself (%s)"
                  % ", ".join(r[1] for r in listed), len(clock_row) == 1)
            if not clock_row:
                return

            before = len(buttons(at, w, row))
            i = clock_row[0]
            q.mouse_goto(CX + 130, CY + ROW_Y0 + i * ROW_H + 3)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(1.5)
            q.mouse_goto(CX + 260, CY + 4)     # "End"
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(3)
            q.mouse_goto(600, 430)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            names = [r[1] for r in rows(at)]
            check("(64) 'End' ends the picked program (%s)"
                  % ", ".join(names),
                  not any("gclock" in n for n in names) and
                  len(buttons(at, w, row)) == before - 1)
        finally:
            q.close()
    finally:
        shutil.rmtree(d, ignore_errors=True)
        for f in (iso, slog, disk):
            if os.path.exists(f):
                os.unlink(f)


def main():
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as f:
        f.truncate(16 * 1024 * 1024)
    slog = tempfile.mktemp(suffix=".log")

    try:
        q = Qemu(KERNEL, INITRD, disk, serial=slog)
        try:
            check("the desktop came up", q.wait_desktop(timeout=40))
            q.mouse_home()

            q.mouse_goto(24, 352)              # the "Tasks" icon
            q.mouse_click()
            time.sleep(4)

            # Out of the way of the cascade: every window opens at the
            # next step of it, and one opened later would stand over
            # the list we are reading. A person moves the window; so
            # does the test.
            global CX, CY
            q.mouse_goto(WX + 100, WY + 4)
            time.sleep(0.4)
            q.mouse_button(True)
            time.sleep(0.4)
            q.mouse_goto(400, 240)
            time.sleep(0.5)
            q.mouse_goto(400, 240)
            time.sleep(0.5)
            q.mouse_button(False)
            time.sleep(1.5)
            CX, CY = 300 + 1, 236 + CONTENT_DY
            q.mouse_goto(600, 430)
            time.sleep(1.5)

            w, h, at = q.screen_raw()
            row = h - TB_H + 2
            listed = rows(at)
            names = [r[1] for r in listed]
            check("the task manager shows the list (%d rows: %s)"
                  % (len(listed), ", ".join(names)), len(listed) >= 3)
            check("it sees itself", any("gtasks" in n for n in names))
            check("and the desktop thread that draws it all",
                  any("desktop" in n or "desktop" in n for n in names))
            # A process has memory of its own; a kernel thread has not -
            # and the list says which is which
            kinds = {r[1]: r[3] for r in listed}
            check("it tells a program from a kernel thread (%s)"
                  % ", ".join("%s=%s" % (k, v) for k, v in kinds.items()),
                  any(v == "program" for v in kinds.values()) and
                  any(v == "kernel" for v in kinds.values()))

            # --- the list is live ---------------------------------------
            q.mouse_goto(24, 72)               # the "Clock" icon
            q.mouse_click()
            time.sleep(4)
            q.mouse_goto(600, 430)
            time.sleep(2)
            w, h, at = q.screen_raw()
            listed = rows(at)
            names = [r[1] for r in listed]
            check("a program started after the window opened appears by itself "
                  "(%s)" % ", ".join(names),
                  any("gclock" in n for n in names))

            clock_row = [i for i, r in enumerate(listed) if "gclock" in r[1]]
            check("and it is described as a program", clock_row and
                  listed[clock_row[0]][3] == "program")

            # --- choosing and ending ------------------------------------
            i = clock_row[0]
            q.mouse_goto(CX + 130, CY + ROW_Y0 + i * ROW_H + 3)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(1.5)
            # The pointer off the row before looking: the cursor is
            # drawn white and, left standing on the row, it hides the
            # very colour that says the row was chosen.
            q.mouse_goto(150, 200)
            time.sleep(1)
            w, h, at = q.screen_raw()
            # By NAME, not by number: the list is read again twice a
            # second and a row can move when a task is born or dies -
            # what was chosen is a task, not a place on the screen.
            got = picked_row(at)
            listed = rows(at)
            check("a click on a row picks that task (row %d: %s)"
                  % (got, listed[got][1] if 0 <= got < len(listed) else "-"),
                  0 <= got < len(listed) and "gclock" in listed[got][1])

            before = len(buttons(at, w, row))
            q.mouse_goto(CX + 260, CY + 4)     # the "End" button
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(3)
            q.mouse_goto(600, 430)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            names = [r[1] for r in rows(at)]
            check("'End' ends the picked program (%s)"
                  % ", ".join(names),
                  not any("gclock" in n for n in names))
            after = len(buttons(at, w, row))
            check("and its window goes from the taskbar (%d -> %d)"
                  % (before, after), after == before - 1)

            # --- what it will not do ------------------------------------
            listed = rows(at)
            kernel_rows = [i for i, r in enumerate(listed)
                           if r[3] == "kernel"]
            check("there are kernel threads on the list", len(kernel_rows) > 0)
            i = kernel_rows[0]
            q.mouse_goto(CX + 130, CY + ROW_Y0 + i * ROW_H + 3)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(1)
            q.mouse_goto(CX + 260, CY + 4)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(2)
            q.mouse_goto(600, 430)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            note = field(at, 4, 190 - 10, 34)
            check("it does not end a kernel thread, and says why ('%s')" % note,
                  "a kernel thread" in note)
            after_kernel = [r[1] for r in rows(at)]
            check("and the thread is still on the list",
                  listed[i][1] in after_kernel)

            # And it will not end itself - a task manager that vanishes
            # when clicked is a joke rather than a tool
            listed = rows(at)
            mine = [i for i, r in enumerate(listed) if "gtasks" in r[1]]
            check("the manager sees its own row", len(mine) == 1)
            i = mine[0]
            q.mouse_goto(CX + 130, CY + ROW_Y0 + i * ROW_H + 3)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(1)
            q.mouse_goto(CX + 260, CY + 4)
            time.sleep(0.5)
            q.mouse_click()
            time.sleep(2)
            q.mouse_goto(600, 430)
            time.sleep(1.5)
            w, h, at = q.screen_raw()
            note = field(at, 4, 190 - 10, 34)
            check("it does not end itself ('%s')" % note, "myself" in note)
            check("and it still runs - the list is on the screen",
                  any("gtasks" in n for n in [r[1] for r in rows(at)]))
        finally:
            q.close()
    finally:
        for f in (disk, slog):
            if os.path.exists(f):
                os.unlink(f)

    print("--- the same manager on the 64-bit kernel ---")
    sixty_four()
    return summary()


sys.exit(main())
