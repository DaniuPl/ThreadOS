#!/usr/bin/env python3
"""ThreadOS smoke test: the native Code project workspace."""

import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, _kernel_define as define


def ed(name):
    return define("user/gedit.c", name)


CODE_W, CODE_H = ed("IDE_W0"), ed("IDE_H0")
SIDE_W, HEADER_H = ed("IDE_SIDE_W"), ed("IDE_HEADER_H")
EDIT_TOP, ROW_H = ed("IDE_EDIT_TOP"), ed("ROW_H")
LIST_ROW_H, TEXT_X = ed("IDE_LIST_ROW_H"), ed("IDE_TEXT_X")
PAD, STATUS_H = ed("PAD"), ed("STATUS_H")
TEXT_BASE = ed("TEXT_BASE")
CELL, TEXT_PX = ed("CELL"), ed("TEXT_PX")
WX, WY = cascade(0, CODE_W + 2, CODE_H + TITLE_H + 2)
CX, CY = WX + 1, WY + CONTENT_DY


def new_file(q, name, contents):
    q.mouse_goto(CX + CODE_W - 197, CY + HEADER_H // 2)
    time.sleep(0.3)
    q.mouse_click()
    q.type_line(name, enter=False)
    q.sendkey("ret")
    q.type_line(contents, enter=False)
    q.sendkey("ctrl-s")
    time.sleep(1)


def project_rows(q):
    shot = q.screen_raw()
    visible = (CODE_H - EDIT_TOP - STATUS_H) // LIST_ROW_H
    rows = []
    for row in range(visible):
        top = EDIT_TOP + row * LIST_ROW_H
        baseline = CY + ui_font_baseline_in(10, top, LIST_ROW_H)
        rows.append(q.read_ui(CX + 10, baseline, SIDE_W - 18, px=10,
                              fg=theme("dark")["app_text"], screen=shot))
    return rows


def code_row(q, row):
    w, h, at = q.screen_raw()
    return Qemu.read_cells(
        None, CX + TEXT_X, CY + EDIT_TOP + row * ROW_H, 40, 1,
        cell=CELL, row_h=ROW_H, base=TEXT_BASE, px=TEXT_PX,
        screen=(w, h, at))[0]


def wait_code(q, timeout=12):
    deadline = time.time() + timeout
    title = ""
    while time.time() < deadline:
        title = q.window_title(WX, WY, CODE_W + 2)
        if title == "Code":
            return True
        time.sleep(0.25)
    return title == "Code"


def click_project_row(q, row):
    q.mouse_goto(CX + 18, CY + EDIT_TOP + row * LIST_ROW_H + 8)
    time.sleep(0.3)
    q.mouse_click()
    time.sleep(1)


def open_code(q, is64=False):
    q.launch_dock(9)
    check("Code opens from the dock", wait_code(q))

    new_file(q, "main.c", "int main() {")
    new_file(q, "util.h", "#define VALUE 7")
    rows = project_rows(q)
    main_row = next((i for i, value in enumerate(rows)
                     if "main.c" in value), -1)
    util_row = next((i for i, value in enumerate(rows)
                     if "util.h" in value), -1)
    check("the project explorer lists source files",
          main_row >= 0 and util_row >= 0)
    if main_row < 0 or util_row < 0:
        return

    click_project_row(q, main_row)
    text = code_row(q, 0)
    check("opening a project file loads it into the editor",
          "int main() {" in text)

    w, h, at = q.screen_raw()
    accent = theme("dark")["accent"]
    highlighted = sum(
        1 for y in range(CY + EDIT_TOP, CY + EDIT_TOP + ROW_H)
        for x in range(CX + TEXT_X, CX + TEXT_X + 3 * CELL)
        if near(at(x, y), accent, 4))
    check("C keywords receive syntax highlighting", highlighted > 2)

    click_project_row(q, util_row)
    check("opening another file adds a tab and changes the source",
          "#define VALUE 7" in code_row(q, 0))

    q.mouse_goto(CX + SIDE_W + 55, CY + HEADER_H + 10)
    time.sleep(0.3)
    q.mouse_click()
    time.sleep(1)
    check("the first tab switches back to its saved document",
          "int main() {" in code_row(q, 0))

    if not is64:
        q.sendkey("end")
        q.sendkey("ret")
        q.type_line("return 0;", enter=False)
        time.sleep(1)
        check("Enter indents the next line inside a C block",
              code_row(q, 1).startswith("    return 0;"))
        q.sendkey("ctrl-s")
        time.sleep(1)
        shot = q.screen_raw()
        status_y = CY + ui_font_baseline_in(11, CODE_H - STATUS_H, STATUS_H)
        status = q.read_ui(CX + 10, status_y, CODE_W - 20, px=11,
                           fg=theme("dark")["app_text"], screen=shot)
        check("Ctrl+S saves the edited source", "saved" in status)

    q.mouse_goto(CX + CODE_W - 50, CY + 15)
    time.sleep(0.3)
    q.mouse_click()
    deadline = time.time() + 10
    while time.time() < deadline and q.dock_windows() < 2:
        time.sleep(0.25)
    check("Terminal opens alongside the project editor",
          q.dock_windows() >= 2)


def test32():
    disk = tempfile.mktemp(suffix=".img")
    slog = tempfile.mktemp(suffix=".log")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    q = Qemu(KERNEL, INITRD, disk=disk, serial=slog)
    try:
        check("the 32-bit desktop starts", q.wait_desktop(timeout=40))
        q.mouse_home()
        open_code(q)
    finally:
        q.close()
        for path in (disk, slog):
            if os.path.exists(path):
                os.unlink(path)


def test64():
    if KERNEL64 is None or not os.path.exists(KERNEL64):
        print("SKIPPED (64): no 64-bit image")
        return
    if shutil.which("grub-mkrescue") is None or \
            shutil.which("qemu-system-x86_64") is None:
        print("SKIPPED (64): no GRUB rescue tools or QEMU")
        return

    work = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    disk = tempfile.mktemp(suffix=".img")
    slog = tempfile.mktemp(suffix=".log")
    with open(disk, "wb") as fh:
        fh.truncate(16 * 1024 * 1024)
    try:
        os.makedirs(os.path.join(work, "boot", "grub"))
        shutil.copy(KERNEL64, os.path.join(work, "boot", "threados64.bin"))
        shutil.copy(INITRD64, os.path.join(work, "boot", "initrd64.img"))
        with open(os.path.join(work, "boot", "grub", "grub.cfg"), "w") as fh:
            fh.write("insmod all_video\nset gfxpayload=keep\n"
                     "set timeout=0\nset default=0\n"
                     'menuentry "ThreadOS" {\n'
                     "    multiboot2 /boot/threados64.bin\n"
                     "    module2 /boot/initrd64.img initrd64\n}\n")
        made = subprocess.run(
            ["grub-mkrescue", "-o", iso, work],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
        check("the 64-bit test disc builds", made)
        if not made:
            return

        q = Qemu(KERNEL, INITRD, disk=disk, cdrom=iso, serial=slog,
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=60)
            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=30)
            check("the 64-bit desktop starts", q.wait_desktop(timeout=40))
            q.mouse_home()
            open_code(q, is64=True)
        finally:
            q.close()
    finally:
        shutil.rmtree(work, ignore_errors=True)
        for path in (iso, disk, slog):
            if os.path.exists(path):
                os.unlink(path)


def main():
    print("--- ThreadCode project workspace ---")
    test32()
    test64()
    return summary()


if __name__ == "__main__":
    sys.exit(main())
