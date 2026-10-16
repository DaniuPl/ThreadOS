#!/usr/bin/env python3
"""ThreadOS smoke test: the installer on a screen of its own (ginstal).

Installing is not one task among others. While a disk is being written
to, a taskbar and somebody's other windows within reach are not a
convenience - they are a way to click something else by mistake. So
'install' takes the WHOLE screen: the compositor runs with no bar, no
icons, no menu and no other window, the installer is the only program
on it, and the only thing the environment itself offers is restarting
the machine.

This group checks both halves of that: that nothing of the desktop is
there, and that everything the program says can be read off the screen,
in the font the kernel drew it with.

The machine is started from a stick under UEFI, and the disk of the
'other machine' hangs behind an AHCI controller with somebody else's
boot loader on it. What lands there is read back by mtools byte for
byte; and at the end that disk is booted on its own, which is the only
proof that installing installed anything.

Run on its own or through scripts/run_tests.py.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, no64, make_gpt_disk, gpt_intact,
                   neighbours_intact, mtools_ok, fat_format, fat_mkdirs,
                   fat_put, fat_get, fat_dir)

ESP_START = 2048
ESP_SECTORS = 204800

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
LOADER = os.path.join(ROOT, "build", "BOOTX64.EFI")

FOREIGN = b"MZ-this-is-somebody-elses-bootmgfw-" * 300

# The window ginstal creates (user/ginstal.c) and the desktop's title
# bar. Both are checked rather than assumed - a window of a different
# size means the test is reading somewhere else, and it should say so
# instead of quietly reading the wallpaper.
WIN_W = 440
WIN_H = 230
TITLE = (0, 0, 255)          # the title bar of the window with focus
BAR = (211, 211, 211)        # the desktop's bars - which must NOT be here
ICON = (255, 162, 0)         # the desktop's icons - likewise
WINDOW = (227, 227, 227)     # a window's grey


def blob(path):
    with open(path, "rb") as fh:
        return fh.read()


def find_window(q, width=WIN_W):
    """Where the window is, found on the screen rather than assumed.

    Its title bar is a solid band of one colour as wide as the window.
    We look for that run - and the fact that it is exactly as wide as
    the program asked for is itself worth knowing."""
    w, h, _ = q.screen_raw()
    px = q._last_pixels

    def at(x, y):
        o = (y * w + x) * 3
        return (px[o], px[o + 1], px[o + 2])

    for y in range(20, h - 40):
        x = 0
        while x < w:
            if all(abs(at(x, y)[i] - TITLE[i]) <= 60 for i in range(3)):
                start = x
                while x < w and all(abs(at(x, y)[i] - TITLE[i]) <= 60
                                    for i in range(3)):
                    x += 1
                if x - start >= width - 4 and x - start <= width + 4:
                    return start, y
            else:
                x += 1
    return None, None



def wait_text(q, x, y, cols, rows, want, tries=10, line_h=12):
    """Reads the window until the words appear.

    Clicking a button and reading the screen a second and a half later
    is fine on an idle machine and a coin toss when the whole suite is
    running. The window is redrawn when the program gets round to it,
    so we look again rather than assume."""
    text = ""
    for _ in range(tries):
        text = q.read_text(x, y, cols, rows, line_h=line_h)
        if want in text:
            return text
        time.sleep(1.0)
    return text

def main():
    print()
    print("--- the installer in a window (ginstal) ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    if not mtools_ok():
        print("SKIPPED: no mtools")
        return summary()
    if not os.path.exists(LOADER):
        subprocess.run(["make", "efi"], cwd=ROOT, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    if not os.path.exists(LOADER):
        print("SKIPPED: no build/BOOTX64.EFI (apt install clang lld)")
        return summary()
    if not os.path.exists(OVMF_CODE) or not os.path.exists(OVMF_VARS):
        print("SKIPPED: no OVMF firmware")
        return summary()
    if shutil.which("qemu-system-x86_64") is None:
        print("SKIPPED: no qemu-system-x86_64")
        return summary()

    esp = tempfile.mkdtemp()
    target = tempfile.mktemp(suffix=".img")
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")
    nvram = tempfile.mktemp(suffix=".fd")

    want_loader = blob(LOADER)
    want_kernel = blob(KERNEL64)
    want_initrd = blob(INITRD64)

    try:
        os.makedirs(os.path.join(esp, "EFI", "BOOT"))
        shutil.copy(LOADER, os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI"))
        shutil.copy(KERNEL64, os.path.join(esp, "threados64.bin"))
        shutil.copy(INITRD64, os.path.join(esp, "initrd64.img"))

        make_gpt_disk(target)
        made = fat_format(target, ESP_START, ESP_SECTORS, fat32=True,
                          label="LAPTOPESP")
        fat_mkdirs(target, ESP_START, ["/EFI", "/EFI/Microsoft",
                                       "/EFI/Microsoft/Boot"])
        fat_put(target, ESP_START, FOREIGN,
                "/EFI/Microsoft/Boot/bootmgfw.efi")
        check("the foreign machine's disk is ready (GPT + FAT32 from mtools)", made)

        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, esp=esp, disk=target, sata=True,
                 serial=slog, qemu="qemu-system-x86_64",
                 uefi=(OVMF_CODE, nvram))
        try:
            q.wait_serial("The 64-bit system is ready", timeout=120)
            seen = len(q.serial())
            q.type_line("install")
            time.sleep(8)
            check("'install' takes the screen instead of refusing",
                  "the desktop is running" not in q.serial()[seen:])

            # The pointer is drawn over the window, and a glyph with
            # half a cursor on it is a glyph nothing can read. So it
            # goes out of the way first - a person moves it too.
            q.mouse_goto(900, 600)
            time.sleep(0.8)

            # --- what is NOT on this screen. The taskbar, the icons and
            # the menu button are the desktop, and the whole point of
            # this mode is that none of them is within reach.
            w, h, at = q.screen_raw()
            bar = sum(1 for x in range(0, w, 8)
                      if all(abs(at(x, h - 6)[i] - BAR[i]) <= 25
                             for i in range(3)))
            check(f"there is no taskbar at the bottom ({bar} samples in the "
                  f"of the bar)", bar < 4)
            top = sum(1 for x in range(0, w, 8)
                      if all(abs(at(x, 3)[i] - BAR[i]) <= 25
                             for i in range(3)))
            check(f"nor the system's top bar ({top} samples)", top < 4)
            icon = sum(1 for x in range(10, 40) for y in range(20, 380)
                       if all(abs(at(x, y)[i] - ICON[i]) <= 30
                              for i in range(3)))
            check(f"nor the desktop icons ({icon} icon pixels)", icon == 0)

            # --- and what IS: the screen says what it is doing, and
            # offers exactly one way out of itself
            head = q.read_text(8, 8, 24, 1)
            check("the screen says what it is", "ThreadOS - installation" in head)
            # the button sits six pixels off the right edge (kernel/desktop.c)
            btn = q.read_text(w - 152 - 6 + 8, 9, 17, 1)
            check("and it offers the only way out: restarting the machine",
                  "Restart" in btn)

            wx, wy = find_window(q)
            check(f"the installer's window is as wide as it asked for "
                  f"({WIN_W} px)", wx is not None)
            if wx is None:
                return summary()
            # The title bar is drawn one pixel inside the frame, and
            # the content one pixel under it (kernel/desktop.c)
            cx, cy = wx, wy + TITLE_H

            # Everything below is read off the screen, in the font the
            # kernel drew it with - not from the serial port
            text = q.read_text(cx + 6, cy + 4, 54, 16, line_h=12)
            check("the window says what it is for",
                  "Installing ThreadOS onto this machine's disk" in text)
            check("and what it found: a disk behind an AHCI controller",
                  "AHCI" in text and "partitions: 3" in text)
            check("and the EFI partition with its label",
                  "FAT32 LAPTOPESP" in text and "#1" in text)
            check("it lists all three files it will write",
                  "/EFI/ThreadOS/BOOTX64.EFI" in text
                  and "/EFI/ThreadOS/threados64.bin" in text
                  and "/EFI/ThreadOS/initrd64.img" in text)
            check("the kernel size on the screen matches the file",
                  str(len(want_kernel)) in text)
            check("it says /EFI/BOOT/BOOTX64.EFI is free",
                  "/EFI/BOOT/BOOTX64.EFI" in text and "free" in text)
            check("and it promises what it will not touch",
                  "I do NOT touch the partition table" in text)

            # Nothing has been written yet - the window has only read
            check("until somebody clicks, nothing on the disk has changed",
                  fat_get(target, ESP_START,
                          "/EFI/ThreadOS/threados64.bin") is None)

            # Two buttons at the bottom, because there are two ways of
            # installing and a way that is reached by first refusing
            # the other one is a way nobody finds. The left one
            # installs beside; the right one takes the whole disk.
            BTN_W, GAP = 168, 10
            by = cy + WIN_H - 16 - 6 + 8
            left = cx + (WIN_W - (2 * BTN_W + GAP)) // 2 + BTN_W // 2
            right = left + BTN_W + GAP

            q.mouse_goto(right, by)
            q.mouse_click()
            text = wait_text(q, cx + 6, cy + 4, 54, 16,
                             "The whole disk for ThreadOS")
            check("the second button shows the other way: the whole disk",
                  "The whole disk for ThreadOS" in text)
            check("and it says outright what that means",
                  "THIS WILL ERASE EVERYTHING ON THIS DISK" in text)
            check("listing what it will do instead",
                  "partition table (GPT)" in text
                  and "EFI partition (FAT32)" in text
                  and "ThreadOS partition (TFS)" in text)
            check("and still nothing on the disk has changed",
                  fat_get(target, ESP_START,
                          "/EFI/ThreadOS/threados64.bin") is None)

            q.mouse_goto(left, by)        # "Back"
            q.mouse_click()
            text = wait_text(q, cx + 6, cy + 4, 54, 16,
                             "Installing ThreadOS onto this machine's disk")
            check("'Back' goes back to installing beside",
                  "Installing ThreadOS onto this machine's disk" in text)

            q.mouse_goto(left, by)        # "Install beside"
            q.mouse_click()
            text = wait_text(q, cx + 6, cy + 4, 54, 10, "Type YES")
            check("a click on 'Install beside' asks for a word instead of writing",
                  "Type YES" in text)

            # A word that is not the word
            q.type_line("NO")
            time.sleep(1.2)
            text = q.read_text(cx + 6, cy + 4, 54, 10, line_h=12)
            check("another word does not start the writing",
                  "that is not the word YES" in text)
            check("and still nothing has been written",
                  fat_get(target, ESP_START,
                          "/EFI/ThreadOS/threados64.bin") is None)

            # The word, and then the kernel says no: writing needs an
            # admin, and the answer to that is a password
            q.type_line("YES")
            time.sleep(1.5)
            text = q.read_text(cx + 6, cy + 4, 54, 10, line_h=12)
            check("after YES the kernel refuses a program that is not admin",
                  "You have to be admin" in text)

            q.type_line("wrongpassword")
            time.sleep(1.5)
            text = q.read_text(cx + 6, cy + 4, 54, 10, line_h=12)
            check("a wrong password opens nothing", "wrong password" in text)

            q.type_line("thread")
            deadline = time.time() + 60
            done = ""
            while time.time() < deadline:
                done = q.read_text(cx + 6, cy + 4, 54, 10, line_h=12)
                if "Done" in done or "It did not work" in done:
                    break
                time.sleep(0.5)
            check("with the right password the installation runs to the end",
                  "Done" in done)
            check("and it says what follows from that",
                  "will boot ThreadOS off the disk" in done
                  and "Nothing of anybody else's was touched" in done)

            # A click where the desktop's menu button would be does
            # nothing - there is no menu to open, and that is the
            # difference between a kiosk and a desktop with one window
            q.mouse_goto(10, 790)
            q.mouse_click()
            time.sleep(1.0)
            w, h, at = q.screen_raw()
            menu = sum(1 for x in range(4, 120) for y in range(h - 130, h - 20)
                       if all(abs(at(x, y)[i] - WINDOW[i]) <= 20
                              for i in range(3)))
            check(f"a click in the corner where the desktop has 'Menu' "
                  f"opens nothing ({menu} menu pixels)", menu == 0)
        finally:
            q.close()

        # --- What is really on that disk, according to somebody else
        for path, want in (("/EFI/ThreadOS/BOOTX64.EFI", want_loader),
                           ("/EFI/ThreadOS/threados64.bin", want_kernel),
                           ("/EFI/ThreadOS/initrd64.img", want_initrd),
                           ("/EFI/BOOT/BOOTX64.EFI", want_loader)):
            got = fat_get(target, ESP_START, path)
            check(f"{path} byte for byte ({len(want)} B)",
                  got is not None and got == want)
        check("somebody elses bootmgfw.efi is untouched",
              fat_get(target, ESP_START,
                      "/EFI/Microsoft/Boot/bootmgfw.efi") == FOREIGN)
        check("the GPT is untouched", gpt_intact(target))
        check("the neighbouring Windows partition is untouched",
              neighbours_intact(target, which=(1,)))

        # --- The proof: the disk boots on its own
        shutil.copy(OVMF_VARS, nvram)
        q2 = Qemu(KERNEL, INITRD, disk=target, sata=True, serial=slog2,
                  qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            out = q2.wait_serial("threadboot: ThreadOS", timeout=60)
            check("a disk installed from the window boots on its own", "threadboot" in out)
            out = q2.wait_serial("The 64-bit system is ready", timeout=120)
            check("and the system on it reaches the shell",
                  "Multiboot OK (Multiboot 2)" in out)
        finally:
            q2.close()
    finally:
        shutil.rmtree(esp, ignore_errors=True)
        for f in (target, slog, slog2, nvram):
            if os.path.exists(f):
                os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
