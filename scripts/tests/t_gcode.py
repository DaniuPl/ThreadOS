#!/usr/bin/env python3
"""ThreadOS smoke test: the native Code project workspace."""

import os
import shutil
import subprocess
import sys
import tempfile
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, _kernel_define as define


def ed(name):
    return define("user/gedit.c", name)


CODE_W, CODE_H = ed("IDE_W0"), ed("IDE_H0")
SIDE_W, HEADER_H = ed("IDE_SIDE_W"), ed("IDE_HEADER_H")
ACTIVITY_W, TAB_H = ed("IDE_ACTIVITY_W"), ed("IDE_TAB_H")
EDIT_TOP, ROW_H = ed("IDE_EDIT_TOP"), ed("ROW_H")
LIST_ROW_H, TEXT_X = ed("IDE_LIST_ROW_H"), ed("IDE_TEXT_X")
PAD, STATUS_H = ed("PAD"), ed("STATUS_H")
TEXT_BASE = ed("TEXT_BASE")
CELL, TEXT_PX = ed("CELL"), ed("TEXT_PX")
TERM_H = ed("IDE_TERM_H")
TERM_HEADER_H = ed("IDE_TERM_HEADER_H")
TERM_TOP = CODE_H - STATUS_H - TERM_H
TERM_ROWS = (TERM_H - TERM_HEADER_H) // ROW_H
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
        rows.append(q.read_ui(CX + ACTIVITY_W + 27, baseline,
                              SIDE_W - ACTIVITY_W - 35, px=10,
                              fg=theme("dark")["app_text"], screen=shot))
    return rows


def code_row(q, row):
    w, h, at = q.screen_raw()
    return Qemu.read_cells(
        None, CX + TEXT_X, CY + EDIT_TOP + row * ROW_H, 40, 1,
        cell=CELL, row_h=ROW_H, base=TEXT_BASE, px=TEXT_PX,
        screen=(w, h, at))[0]


def terminal_rows(q):
    return q.read_cells(
        CX + SIDE_W + 10, CY + TERM_TOP + TERM_HEADER_H,
        (CODE_W - SIDE_W - 20) // CELL, TERM_ROWS,
        cell=CELL, row_h=ROW_H, base=TEXT_BASE, px=TEXT_PX)


def visible_project_path(q):
    return q.read_ui(CX + ACTIVITY_W + 10, CY + EDIT_TOP - 4,
                     width=SIDE_W - ACTIVITY_W - 18, px=9,
                     face="mono").strip()


def wait_code(q, timeout=12):
    global WX, WY, CX, CY
    width, height = q.size()
    frame_w, frame_h = CODE_W + 2, CODE_H + TITLE_H + 2
    bottom = work_bottom(h=height)
    positions = []
    for slot in range(8):
        wx, wy = cascade(slot, frame_w, frame_h, w=width, h=height)
        if wx + frame_w > width:
            wx = max(0, width - frame_w)
        if wy + frame_h > bottom:
            wy = max(0, bottom - frame_h)
        if (wx, wy) not in positions:
            positions.append((wx, wy))

    deadline = time.time() + timeout
    while time.time() < deadline:
        for wx, wy in positions:
            title = q.window_title(wx, wy, CODE_W + 2)
            if title in ("Code", "Kod"):
                WX, WY = wx, wy
                CX, CY = WX + 1, WY + CONTENT_DY
                return True
        time.sleep(0.25)
    return False


def click_project_row(q, row):
    q.mouse_goto(CX + ACTIVITY_W + 18,
                 CY + EDIT_TOP + row * LIST_ROW_H + 8)
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

    def ink(x0, x1):
        pixels = Counter(at(x, y)
                         for y in range(CY + EDIT_TOP, CY + EDIT_TOP + ROW_H)
                         for x in range(x0, x1))
        background = pixels.most_common(1)[0][0]
        colors = [(count, color) for color, count in pixels.items()
                  if color != background]
        return max(colors)[1] if colors else None

    keyword = ink(CX + TEXT_X, CX + TEXT_X + 3 * CELL)
    plain = ink(CX + TEXT_X + 4 * CELL, CX + TEXT_X + 8 * CELL)
    check("C keywords receive syntax highlighting",
          keyword is not None and plain is not None and keyword != plain)

    click_project_row(q, util_row)
    check("opening another file adds a tab and changes the source",
          "#define VALUE 7" in code_row(q, 0))

    q.mouse_goto(CX + SIDE_W + 55, CY + HEADER_H + 10)
    time.sleep(0.3)
    q.mouse_click()
    time.sleep(1)
    check("the first tab switches back to its saved document",
          "int main() {" in code_row(q, 0))

    q.sendkey("ctrl-o")
    time.sleep(1)
    q.type_line("util", enter=False)
    time.sleep(0.5)
    title, quick_rows = q.ug_pick_read(CX, CY, CODE_H)
    check("Quick Open filters project files as you type",
          "Quick Open" in title and
          any("util.h" in row for row in quick_rows) and
          not any("main.c" in row for row in quick_rows))
    q.sendkey("ret")
    time.sleep(1)
    check("Enter opens the first matching project file",
          "#define VALUE 7" in code_row(q, 0))

    if not is64:
        q.mouse_goto(CX + SIDE_W + 55, CY + HEADER_H + 10)
        time.sleep(0.3)
        q.mouse_click()
        time.sleep(0.5)
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
                           screen=shot)
        check("Ctrl+S saves the edited source", "saved" in status)

    windows_before = q.dock_windows()
    q.mouse_goto(CX + CODE_W - 50, CY + 15)
    time.sleep(0.3)
    q.mouse_click()
    deadline = time.time() + 10
    rows = terminal_rows(q)
    while time.time() < deadline and not any("ush" in row for row in rows):
        time.sleep(0.25)
        rows = terminal_rows(q)
    check("Terminal opens inside the project editor",
          q.dock_windows() == windows_before and
          any("ush" in row for row in rows))

    input_y = TERM_TOP + TERM_H - ROW_H // 2
    expected_path = visible_project_path(q)
    q.mouse_goto(CX + SIDE_W + 100, CY + input_y)
    time.sleep(0.3)
    q.mouse_click()
    # ush64 has no pwd builtin. uls64 lists the working directory, but its
    # output is taller than the panel, so only the tail stays visible: the
    # project-only files and the exit line prove where it ran.
    q.type_line("uls64" if is64 else "pwd")
    deadline = time.time() + 10
    rows = terminal_rows(q)

    def ran_in_project(rows):
        if is64:
            text = "\n".join(rows)
            return (".code-term.out" in text and "uls64: entries" in text and
                    "ush64: done, code 0" in text)
        return (any(row.startswith("ush:") and row.rstrip().endswith("pwd")
                    for row in rows) and
                any(row.strip().startswith(expected_path) for row in rows))

    while time.time() < deadline and not ran_in_project(rows):
        time.sleep(0.25)
        rows = terminal_rows(q)
    check("the embedded shell accepts input and runs in the project directory",
          expected_path.startswith("/") and len(expected_path) >= 4 and
          ran_in_project(rows))

    q.mouse_goto(CX + SIDE_W + 80, CY + EDIT_TOP + 10)
    time.sleep(0.3)
    q.mouse_click()
    q.sendkey("ctrl-o")
    time.sleep(0.5)
    q.type_line("code-term", enter=False)
    time.sleep(0.5)
    title, quick_rows = q.ug_pick_read(CX, CY, CODE_H)
    check("terminal capture files stay hidden from Quick Open",
          "Quick Open" in title and
          not any("code-term" in row for row in quick_rows))
    q.sendkey("esc")


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
