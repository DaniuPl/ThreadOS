#!/usr/bin/env python3
"""ThreadOS smoke test: the installer on a screen of its own (ginstal).

Installing is not one task among others. While a disk is being written
to, a dock and somebody's other windows within reach are not a
convenience - they are a way to click something else by mistake. So
'install' takes the WHOLE screen: the compositor runs with no bar, no
dock, no menu and no other window, the installer is the only program
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
WIN_H = 310
WINDOW = theme("dark")["win_bg"]
# The text sits on a grid of rows (user/ginstal.c): ROW_H apart from
# ROW_Y0 down, starting PAD from the left
ROW_H, ROW_Y0, PAD = 16, 10, 14
BTN_W, BTN_H, BTN_GAP = 168, 26, 10
# The colours text is in: plain, quiet, a warning
INKS = [theme("dark")[k] for k in ("app_text", "app_dim", "app_danger")]


def blob(path):
    with open(path, "rb") as fh:
        return fh.read()


def find_window(q, width=WIN_W, height=WIN_H):
    """Where the window is: the middle of the screen, where the kiosk
    puts it (kernel/desktop.c) - and that is checked rather than
    assumed: its content is grey from its first column to its last and
    not one column further either way, which is also how we know it is
    exactly as wide as the program asked for. Returns the window's
    corner, or (None, None)."""
    w, h, at = q.screen_raw()
    ww, wh = width + 2, height + TITLE_H + 2
    x, y = (w - ww) // 2, max((h - wh) // 2, 30)
    row = y + CONTENT_DY + height // 2

    def grey(px):
        return near(px, WINDOW, 3)
    if grey(at(x + 1, row)) and grey(at(x + width, row)) and \
       not grey(at(x - 1, row)) and not grey(at(x + width + 2, row)):
        return x, y
    return None, None


def read_rows(q, cx, cy, rows=16):
    """The window's text, a line per row of its grid, read in the
    lettering the program wrote it in"""
    return q.read_ui_lines(cx + PAD - 4, cx + WIN_W - 4,
                           [cy + ui_font_baseline_in(UD_PX, ROW_Y0 + r * ROW_H,
                                                     ROW_H)
                            for r in range(rows)], px=UD_PX, colours=INKS)


def wait_text(q, cx, cy, want, tries=10, rows=16):
    """Reads the window until the words appear.

    Clicking a button and reading the screen a second and a half later
    is fine on an idle machine and a coin toss when the whole suite is
    running. The window is redrawn when the program gets round to it,
    so we look again rather than assume."""
    text = ""
    for _ in range(tries):
        text = read_rows(q, cx, cy, rows)
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
            q.type_line("sudo set language pl")
            q.command("thread", "saved", timeout=30)
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

            # --- what is NOT on this screen. The menu bar and the dock
            # are the desktop, and the whole point of this mode is that
            # neither of them is within reach: along the top and the
            # bottom there is nothing but the aurora.
            w, h, at = q.screen_raw()
            bottom = sum(1 for x in range(0, w, 8)
                         if at(x, h - 20) == aurora_px(x, h - 20, w, h))
            check(f"there is no dock at the bottom ({bottom} of {w // 8} "
                  f"samples are the bare aurora)", bottom == w // 8)
            top = sum(1 for x in range(0, w, 8)
                      if at(x, 3) == aurora_px(x, 3, w, h))
            check(f"nor the menu bar ({top} of {w // 8} samples)",
                  top == w // 8)

            # --- and what IS: the screen says what it is doing, and
            # offers exactly one way out of itself
            head = q.read_ui(14, ui_font_baseline_in(BAR_PX + 2, 8, 20),
                             width=260, px=BAR_PX + 2, face="bold",
                             fg=(255, 255, 255),
                             under=lambda x, y: aurora_px(x, y, w, h))
            check("the screen says what it is ('%s')" % head,
                  "ThreadOS — instalacja" in head)
            # the button sits eight pixels off the right edge
            # (kernel/desktop.c), 152 wide
            bx = w - 152 - 8
            btn = q.find_ui(bx + 12, bx + 140, ui_baseline_in(8, 20),
                            fg=(255, 255, 255),
                            under=frost_ref(bx, 8, 152, 20, 6, 0xFFFFFF, 50,
                                            w, h))
            check("and it offers the only way out: restarting the machine "
                  "('%s')" % btn, "Uruchom ponownie" in btn)

            wx, wy = find_window(q)
            check(f"the installer's window is in the middle, as wide as it "
                  f"asked for ({WIN_W} px)", wx is not None)
            if wx is None:
                return summary()
            cx, cy = wx + 1, wy + CONTENT_DY

            # Everything below is read off the screen, in the font the
            # kernel drew it with - not from the serial port
            text = read_rows(q, cx, cy)
            check("the window says what it is for",
                  "Instalowanie ThreadOS na dysku tego komputera" in text)
            check("and what it found: a disk behind an AHCI controller",
                  "AHCI" in text and "partycje: 3" in text)
            check("and the EFI partition with its label",
                  "FAT32 LAPTOPESP" in text and "nr 1" in text)
            check("it lists all three files it will write",
                  "/EFI/ThreadOS/BOOTX64.EFI" in text
                  and "/EFI/ThreadOS/threados64.bin" in text
                  and "/EFI/ThreadOS/initrd64.img" in text)
            check("the kernel size on the screen matches the file",
                  str(len(want_kernel)) in text)
            check("it says /EFI/BOOT/BOOTX64.EFI is free",
                  "/EFI/BOOT/BOOTX64.EFI" in text and "wolne" in text)
            check("and it promises what it will not touch",
                  "Nie zmieniam tablicy" in text)

            # Nothing has been written yet - the window has only read
            check("until somebody clicks, nothing on the disk has changed",
                  fat_get(target, ESP_START,
                          "/EFI/ThreadOS/threados64.bin") is None)

            # Two buttons at the bottom, because there are two ways of
            # installing and a way that is reached by first refusing
            # the other one is a way nobody finds. The left one
            # installs beside; the right one takes the whole disk.
            by = cy + WIN_H - BTN_H - 12 + BTN_H // 2
            left = cx + (WIN_W - (2 * BTN_W + BTN_GAP)) // 2 + BTN_W // 2
            right = left + BTN_W + BTN_GAP

            q.mouse_goto(right, by)
            q.mouse_click()
            text = wait_text(q, cx, cy, "Cały dysk dla ThreadOS")
            check("the second button shows the other way: the whole disk",
                  "Cały dysk dla ThreadOS" in text)
            check("and it says outright what that means",
                  "TO USUNIE WSZYSTKO Z TEGO DYSKU" in text)
            check("listing what it will do instead",
                  "partition table (GPT)" in text
                  and "EFI partition (FAT32)" in text
                  and "ThreadOS partition (TFS)" in text)
            check("and still nothing on the disk has changed",
                  fat_get(target, ESP_START,
                          "/EFI/ThreadOS/threados64.bin") is None)

            q.mouse_goto(left, by)        # "Back"
            q.mouse_click()
            text = wait_text(q, cx, cy,
                             "Instalowanie ThreadOS na dysku tego komputera")
            check("'Back' goes back to installing beside",
                  "Instalowanie ThreadOS na dysku tego komputera" in text)

            q.mouse_goto(left, by)        # "Install beside"
            q.mouse_click()
            text = wait_text(q, cx, cy, "Wpisz YES")
            check("a click on 'Install beside' asks for a word instead of writing",
                  "Wpisz YES" in text)

            # A word that is not the word
            q.type_line("NO")
            time.sleep(1.2)
            text = read_rows(q, cx, cy, 10)
            check("another word does not start the writing",
                  "to nie jest wymagane słowo YES" in text)
            check("and still nothing has been written",
                  fat_get(target, ESP_START,
                          "/EFI/ThreadOS/threados64.bin") is None)

            # The confirmation word authorizes this live-media install;
            # it must not ask for an unrelated kernel account password.
            q.type_line("YES")
            deadline = time.time() + 60
            done = ""
            while time.time() < deadline:
                done = read_rows(q, cx, cy, 10)
                if "Gotowe" in done or "Operacja się nie powiodła" in done:
                    break
                time.sleep(0.5)
            check("after YES the installation runs to the end without a "
                  "kernel password", "Gotowe" in done
                  and "Wymagane są uprawnienia administratora" not in done)
            check("and it says what follows from that: ThreadOS is in the "
                  "firmware's boot menu",
                  "menu startowym tego komputera" in done
                  and "Nie zmieniono cudzych danych" in done)

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
