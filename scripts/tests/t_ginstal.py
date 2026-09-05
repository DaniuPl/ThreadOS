#!/usr/bin/env python3
"""ProtonOS smoke test: the installer on a screen of its own (ginstal).

Installing is not one task among others. While a disk is being written
to, a taskbar and somebody's other windows within reach are not a
convenience - they are a way to click something else by mistake. So
'instaluj' takes the WHOLE screen: the compositor runs with no bar, no
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

FOREIGN = b"MZ-to-jest-cudzy-bootmgfw-" * 300

# The window ginstal creates (user/ginstal.c) and the desktop's title
# bar. Both are checked rather than assumed - a window of a different
# size means the test is reading somewhere else, and it should say so
# instead of quietly reading the wallpaper.
WIN_W = 440
WIN_H = 218
TITLE_H = 10                 # kernel/desktop.c
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
    px = q._ostatnie_piksele

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


def main():
    print()
    print("--- instalator w oknie (ginstal) ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()
    if not mtools_ok():
        print("POMINIETE: brak mtools")
        return summary()
    if not os.path.exists(LOADER):
        subprocess.run(["make", "efi"], cwd=ROOT, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    if not os.path.exists(LOADER):
        print("POMINIETE: brak build/BOOTX64.EFI (apt install clang lld)")
        return summary()
    if not os.path.exists(OVMF_CODE) or not os.path.exists(OVMF_VARS):
        print("POMINIETE: brak firmware OVMF")
        return summary()
    if shutil.which("qemu-system-x86_64") is None:
        print("POMINIETE: brak qemu-system-x86_64")
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
        shutil.copy(KERNEL64, os.path.join(esp, "protonos64.bin"))
        shutil.copy(INITRD64, os.path.join(esp, "initrd64.img"))

        make_gpt_disk(target)
        made = fat_format(target, ESP_START, ESP_SECTORS, fat32=True,
                          label="LAPTOPESP")
        fat_mkdirs(target, ESP_START, ["/EFI", "/EFI/Microsoft",
                                       "/EFI/Microsoft/Boot"])
        fat_put(target, ESP_START, FOREIGN,
                "/EFI/Microsoft/Boot/bootmgfw.efi")
        check("dysk obcej maszyny gotowy (GPT + FAT32 od mtools)", made)

        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, esp=esp, disk=target, sata=True,
                 serial=slog, qemu="qemu-system-x86_64",
                 uefi=(OVMF_CODE, nvram))
        try:
            q.wait_serial("System 64-bitowy gotowy", timeout=120)
            seen = len(q.serial())
            q.type_line("instaluj")
            time.sleep(8)
            check("'instaluj' bierze ekran, a nie odmawia",
                  "pulpit juz dziala" not in q.serial()[seen:])

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
            check(f"na dole nie ma paska zadan ({bar} probek w kolorze "
                  f"paska)", bar < 4)
            top = sum(1 for x in range(0, w, 8)
                      if all(abs(at(x, 3)[i] - BAR[i]) <= 25
                             for i in range(3)))
            check(f"ani gornego paska systemu ({top} probek)", top < 4)
            icon = sum(1 for x in range(10, 40) for y in range(20, 380)
                       if all(abs(at(x, y)[i] - ICON[i]) <= 30
                              for i in range(3)))
            check(f"ani ikon na pulpicie ({icon} pikseli ikon)", icon == 0)

            # --- and what IS: the screen says what it is doing, and
            # offers exactly one way out of itself
            head = q.read_text(8, 8, 24, 1)
            check("ekran mowi, czym jest", "ProtonOS - instalacja" in head)
            # the button sits six pixels off the right edge (kernel/desktop.c)
            btn = q.read_text(w - 152 - 6 + 8, 9, 17, 1)
            check("i oferuje jedyne wyjscie: restart maszyny",
                  "Uruchom ponownie" in btn)

            wx, wy = find_window(q)
            check(f"okno instalatora ma szerokosc, o ktora poprosil "
                  f"({WIN_W} px)", wx is not None)
            if wx is None:
                return summary()
            # The title bar is drawn one pixel inside the frame, and
            # the content one pixel under it (kernel/desktop.c)
            cx, cy = wx, wy + TITLE_H

            # Everything below is read off the screen, in the font the
            # kernel drew it with - not from the serial port
            text = q.read_text(cx + 6, cy + 4, 54, 15, line_h=12)
            check("okno mowi, po co jest",
                  "Instalacja ProtonOS-a na dysk tej maszyny" in text)
            check("i co znalazlo: dysk za kontrolerem AHCI",
                  "AHCI" in text and "partycji: 3" in text)
            check("oraz partycje EFI z jej etykieta",
                  "FAT32 LAPTOPESP" in text and "nr 1" in text)
            check("wypisuje wszystkie trzy pliki, ktore zapisze",
                  "/EFI/ProtonOS/BOOTX64.EFI" in text
                  and "/EFI/ProtonOS/protonos64.bin" in text
                  and "/EFI/ProtonOS/initrd64.img" in text)
            check("rozmiar jadra na ekranie zgadza sie z plikiem",
                  str(len(want_kernel)) in text)
            check("mowi, ze /EFI/BOOT/BOOTX64.EFI jest wolny",
                  "/EFI/BOOT/BOOTX64.EFI" in text and "wolny" in text)
            check("i obiecuje, czego nie ruszy",
                  "NIE ruszam tablicy partycji" in text)

            # Nothing has been written yet - the window has only read
            check("dopoki nikt nie kliknal, na dysku nic sie nie zmienilo",
                  fat_get(target, ESP_START,
                          "/EFI/ProtonOS/protonos64.bin") is None)

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
            time.sleep(1.5)
            text = q.read_text(cx + 6, cy + 4, 54, 14, line_h=12)
            check("drugi przycisk pokazuje druga droge: caly dysk",
                  "Caly dysk dla ProtonOS-a" in text)
            check("i mowi wprost, co to znaczy",
                  "TO SKASUJE WSZYSTKO NA TYM DYSKU" in text)
            check("wymieniajac, co zrobi zamiast tego",
                  "tablica partycji (GPT)" in text
                  and "partycja EFI (FAT32)" in text
                  and "partycja ProtonOS (PFS)" in text)
            check("a na dysku wciaz nic sie nie zmienilo",
                  fat_get(target, ESP_START,
                          "/EFI/ProtonOS/protonos64.bin") is None)

            q.mouse_goto(left, by)        # "Wroc"
            q.mouse_click()
            time.sleep(1.5)
            text = q.read_text(cx + 6, cy + 4, 54, 14, line_h=12)
            check("'Wroc' wraca do instalacji obok",
                  "Instalacja ProtonOS-a na dysk tej maszyny" in text)

            q.mouse_goto(left, by)        # "Zainstaluj obok"
            q.mouse_click()
            time.sleep(1.5)
            text = q.read_text(cx + 6, cy + 4, 54, 10, line_h=12)
            check("klik w 'Zainstaluj obok' pyta o slowo, a nie zapisuje",
                  "Wpisz TAK" in text)

            # A word that is not the word
            q.type_line("NIE")
            time.sleep(1.2)
            text = q.read_text(cx + 6, cy + 4, 54, 10, line_h=12)
            check("inne slowo nie zaczyna zapisu",
                  "to nie jest slowo TAK" in text)
            check("i nadal nic nie zostalo zapisane",
                  fat_get(target, ESP_START,
                          "/EFI/ProtonOS/protonos64.bin") is None)

            # The word, and then the kernel says no: writing needs an
            # admin, and the answer to that is a password
            q.type_line("TAK")
            time.sleep(1.5)
            text = q.read_text(cx + 6, cy + 4, 54, 10, line_h=12)
            check("po TAK jadro odmawia programowi, ktory nie jest adminem",
                  "Trzeba byc adminem" in text)

            q.type_line("zlehaslo")
            time.sleep(1.5)
            text = q.read_text(cx + 6, cy + 4, 54, 10, line_h=12)
            check("zle haslo nie otwiera niczego", "zle haslo" in text)

            q.type_line("proton")
            deadline = time.time() + 60
            done = ""
            while time.time() < deadline:
                done = q.read_text(cx + 6, cy + 4, 54, 10, line_h=12)
                if "Gotowe" in done or "Nie udalo" in done:
                    break
                time.sleep(0.5)
            check("po dobrym hasle instalacja dochodzi do konca",
                  "Gotowe" in done)
            check("i mowi, co z tego wynika",
                  "uruchomi ProtonOS-a z dysku" in done
                  and "Nic cudzego nie zostalo ruszone" in done)

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
            check(f"klik w rogu, gdzie na pulpicie jest 'Menu', nic nie "
                  f"otwiera ({menu} pikseli menu)", menu == 0)
        finally:
            q.close()

        # --- What is really on that disk, according to somebody else
        for path, want in (("/EFI/ProtonOS/BOOTX64.EFI", want_loader),
                           ("/EFI/ProtonOS/protonos64.bin", want_kernel),
                           ("/EFI/ProtonOS/initrd64.img", want_initrd),
                           ("/EFI/BOOT/BOOTX64.EFI", want_loader)):
            got = fat_get(target, ESP_START, path)
            check(f"{path} co do bajtu ({len(want)} B)",
                  got is not None and got == want)
        check("cudzy bootmgfw.efi nietkniety",
              fat_get(target, ESP_START,
                      "/EFI/Microsoft/Boot/bootmgfw.efi") == FOREIGN)
        check("tablica GPT nietknieta", gpt_intact(target))
        check("sasiednia partycja Windows nietknieta",
              neighbours_intact(target, which=(1,)))

        # --- The proof: the disk boots on its own
        shutil.copy(OVMF_VARS, nvram)
        q2 = Qemu(KERNEL, INITRD, disk=target, sata=True, serial=slog2,
                  qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            out = q2.wait_serial("protoboot: ProtonOS", timeout=60)
            check("dysk zainstalowany z okna startuje sam", "protoboot" in out)
            out = q2.wait_serial("System 64-bitowy gotowy", timeout=120)
            check("i system z niego dochodzi do powloki",
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
