#!/usr/bin/env python3
"""ThreadOS test: Swatch's templates build with clang on ThreadOS and run.

Swatch (user64/gswatch.c) starts a program from one of four templates -
a window with a list, Settings-like, a phone program, a game: a project
in ~/Projects whose Run in Code builds it with clang on the system
itself and starts what it built. clang's files go on the disk this test
boots from as they lie on an installed system (`make clang`, as
t_clang), and then, under Weave, for each template:

  Make it in Swatch, with a name of the test's: Code comes up

  Ctrl+R in Code: its first step is clang on src/main.c, which ends
  well - ThreadOS's headers (user/ugui.h and the rest) found where
  apps/clang/threados.cfg says - and its second starts what was built

  the program's window comes up, the project's name in its title bar,
  read off the screen in Inter's bold

  Alt+F4 closes it: the program ends with code 0, and Code says so;
  Alt+Tab to Code and Alt+F4 close Code, and Swatch is in front again

Skipped without BUILD/clang-sdk, as t_clang is.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, LOOK, _kernel_define

L = LOOK
N = theme("night")
SRC = "user64/gswatch.c"
BUILD = os.path.dirname(os.path.abspath(KERNEL64 or "build/x"))
SDK = os.path.join(BUILD, "clang-sdk")
# the templates in gswatch.c's order, and the projects made from them
TEMPLATES = [("A window with a list", "list-demo"),
             ("Settings-like", "settings-demo"),
             ("A phone program", "phone-demo"),
             ("A game", "loom-game")]


def d(name):
    return _kernel_define(SRC, name)


def make_disk(work):
    tree = os.path.join(work, "tree")
    os.mkdir(tree)
    # cp -a keeps the symbolic links: the compiler is on the disk once,
    # under every name it answers to
    subprocess.run(["cp", "-a", os.path.join(SDK, "usr"), tree], check=True)
    os.mkdir(os.path.join(tree, "tmp"))
    os.chmod(os.path.join(tree, "tmp"), 0o1777)
    size = int(subprocess.run(["du", "-sm", tree], check=True,
                              capture_output=True, text=True).stdout.split()[0])
    img = os.path.join(work, "disk.img")
    subprocess.run(["truncate", "-s", "%dM" % (size + 160), img], check=True)
    subprocess.run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", "4096",
                    "-L", "ThreadOS", "-d", tree, img], check=True)
    return img


def main():
    print("--- Swatch's templates, built and run ---")
    if not os.path.exists(os.path.join(SDK, "done.stamp")):
        print("SKIPPED: no " + SDK + " - make clang")
        return summary()
    work = tempfile.mkdtemp()
    try:
        img = make_disk(work)
        q = Qemu(KERNEL, INITRD, img, sata=True, ram_mib=2048,
                 serial=os.path.join(work, "s.log"))
        try:
            run(q)
        finally:
            q.close()
    finally:
        subprocess.run(["rm", "-rf", work])
    return summary()


def run(q):
    check("the desktop came up", q.wait_desktop())
    W, H = q.size()
    q.click_desktop()
    check("'layout weave'", "saved" in q.sudo("set layout weave", "saved"))
    end = time.time() + 10
    while time.time() < end and "the Weave style" not in q.serial():
        time.sleep(0.3)
    away = (W - 4, H // 2)
    acc = N["accent"]
    R = L["WEAVE_WIN_R"]

    def front(timeout=30):
        """The window in front, by the selvedge only it has in the
        accent: its corner, once the same twice running (a window still
        opening is a picture growing), or None"""
        last = None
        end = time.time() + timeout
        while time.time() < end:
            q.mouse_goto(*away)
            _, _, at = q.screen_raw()
            got = None
            for y in range(0, H - 200):
                for x in range(0, W - 100, 2):
                    if at(x, y) == acc and at(x + 99, y) == acc and \
                            at(x, y + 2) == acc and at(x, y + 3) != acc:
                        got = (x - R, y)
                        break
                if got:
                    break
            if got and got == last:
                return got
            last = got
            time.sleep(0.5)
        return None

    def front_title(timeout=30):
        w = front(timeout)
        if not w:
            return ""
        x0 = w[0] + L["WEAVE_BAR_LEFT"] + L["WEAVE_TITLE_ICON"] + 2
        base = ui_font_baseline_in(L["WEAVE_TITLE_PX"],
                                   w[1] + L["SELVEDGE_H"], L["WEAVE_BAR_H"])
        return q.find_ui(x0, x0 + 260, base, px=L["WEAVE_TITLE_PX"],
                         face="interb")

    def wait_title(want, timeout=90):
        end = time.time() + timeout
        t = ""
        while time.time() < end:
            t = front_title()
            if t.startswith(want):
                return t
            time.sleep(1)
        return t

    q.type_line("runw gswatch")
    out = q.wait_serial("[swatch] drawn", timeout=60)
    m = re.search(r"\[swatch\] drawn (\d+) (\d+)", out)
    cw, ch = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
    sw = front()
    check(f"Swatch is up ({cw}x{ch}, at {sw})", sw and cw > 0)
    if not sw:
        return
    ox, oy = sw[0] + 1, sw[1] + L["WEAVE_TITLE_H"] + 1
    rx = cw - d("RIGHT_W") - d("GAP")
    field_y = d("TPL_Y") + 4 * d("TPL_STEP") + 22

    for i, (tpl, proj) in enumerate(TEMPLATES):
        seen = len(q.serial())
        q.click(ox + rx + 60, oy + d("TPL_Y") + i * d("TPL_STEP") + 20)
        q.click(ox + rx + d("RIGHT_W") // 2, oy + field_y + 14)
        for _ in range(32):
            q.sendkey("backspace")
        q.type_line(proj, enter=False)
        q.click(ox + rx + d("RIGHT_W") // 2, oy + ch - 50 + 18)
        q.mouse_goto(*away)
        out = q.wait_serial("[swatch] made", timeout=20)[seen:]
        m = re.search(r"\[swatch\] made (\S+) from (.+)", out)
        made = (m.group(1), m.group(2).strip()) if m else ("", "")
        check(f"{tpl}: made as {proj} ({made})",
              made[0].endswith("/Projects/" + proj) and made[1] == tpl)
        code = wait_title("Code")
        check(f"{tpl}: Code comes up ({code!r})", code.startswith("Code"))

        seen = len(q.serial())
        q.sendkey("ctrl-r")
        first = "[code] step 1 of 2: /usr/bin/clang -O2 src/main.c -o " + proj
        second = "[code] step 2 of 2: ./" + proj
        end = time.time() + 900
        log = ""
        while time.time() < end:
            log = q.serial()[seen:]
            if second in log or "[code] finished" in log:
                break
            time.sleep(1)
        said = [ln.strip() for ln in log.splitlines()
                if ln.startswith("[code]")]
        check(f"{tpl}: clang builds it on ThreadOS, then it starts ({said})",
              first in log and second in log and
              log.index(first) < log.index(second))
        if second not in log:
            return
        title = wait_title(proj)
        check(f"{tpl}: its window, {proj!r} in the title bar ({title!r})",
              title == proj)

        seen = len(q.serial())
        q.sendkey("alt-f4")
        end = time.time() + 30
        while time.time() < end and \
                "[code] finished" not in q.serial()[seen:]:
            time.sleep(0.5)
        log = q.serial()[seen:]
        check(f"{tpl}: Alt+F4 closes it, and it ends with code 0",
              "[code] finished, exit code 0" in log)
        # a window that goes takes the focus with it: Alt+Tab to Code
        for _ in range(6):
            if front_title(4).startswith("Code"):
                break
            q.sendkey("alt-tab")
            time.sleep(0.5)
        q.sendkey("alt-f4")
        for _ in range(6):
            time.sleep(1)
            if front_title(4).startswith("Swatch"):
                break
            q.sendkey("alt-tab")
        back = wait_title("Swatch", timeout=20)
        check(f"{tpl}: Code closed, Swatch in front again ({back!r})",
              back.startswith("Swatch"))


if __name__ == "__main__":
    sys.exit(main())
