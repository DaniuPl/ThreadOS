#!/usr/bin/env python3
"""ThreadOS smoke test: installing the application packages.

Weft and the web engine's programs do not live in the initrd every boot
carries but in a package beside it (.tapp, scripts/mktapp.py), which
the loader hands over as a module named "tapp". A stick that brought
packages should leave a disk that has them: otherwise the installed
system starts without the browser the stick had, and nobody is told
why.

Packages are optional, and so is writing them. Where they fit they go
into /EFI/ThreadOS/apps, where the installed system's loader looks;
where they do not, the system is installed without them and the plan
says so before anything is written. Three disks, one for each answer:

  - an EFI partition with plenty of room, installed beside: written;
  - an EFI partition nearly full, installed beside: left out, said,
    and the rest of the system written all the same;
  - the whole disk: the EFI partition made large enough to hold them,
    and the installed system, started on its own, its loader finding
    the package in its directory and running a program out of it.

And a package installed on that system while it runs: 'tapp install'
refuses it while it is signed by a key the system does not trust, and
an unsigned one always; once update.key holds the test's key, a double
click on it in the Files app opens gtapp, whose window - read off the
screen - says what it is and that it is signed, and whose Install asks
for the administrator's password and then installs it: written into
/EFI/ThreadOS/apps, its program running at once and its tile in the
dock. 'tapp install' refuses it a second time as the version it has;
mtools reads it back off the disk, and the next start takes it from
there.

What is on the disks afterwards is read by mtools, not asked of the
system that wrote it. The case of a whole disk too small for the
larger partition is in t_wholedisk.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import _kernel_define, _at, MTOOLS_ENV
from ptest import (check, summary, no64, make_gpt_disk, mtools_ok,
                   fat_format, fat_put, fat_get, gpt_read, stick_starts_live,
                   SERVED, start_http_server, cascade, TITLE_H, CONTENT_DY,
                   UD_PX, ui_font_baseline_in, BAR_H)
from mkblob import blob as pattern

ESP_MIB_APPS = _kernel_define("kernel/install.c", "ESP_MIB_APPS")
ESP_START = 2048

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
BUILD = os.path.dirname(os.path.abspath(KERNEL64 or "build/x"))
LOADER = os.path.join(BUILD, "BOOTX64.EFI")
ECHO = os.path.join(BUILD, "user64", "uecho64.elf")

# The program the package carries, under a name the initrd does not
# have: a program that runs under it can only have come out of it
PROBE = "appsecho"
PKG = "probe.tapp"
PKG_PATH = "/EFI/ThreadOS/apps/" + PKG


def blob(path):
    with open(path, "rb") as fh:
        return fh.read()


def make_apps(path):
    """A package (scripts/mktapp.py): a program, and a megabyte of data
    that has to arrive bit for bit"""
    data = os.path.join(os.path.dirname(path), "pattern.bin")
    with open(data, "wb") as fh:
        fh.write(pattern("apps-image", 1 << 20))
    return subprocess.run([sys.executable,
                           os.path.join(ROOT, "scripts", "mktapp.py"), path,
                           "--id", "probe", "--name", "Probe", "--version",
                           "1", "--arch", "x86_64", "--exec", PROBE,
                           f"{PROBE}={ECHO}", f"pattern.bin={data}"],
                          capture_output=True).returncode == 0


def make_stick(esp, apps):
    os.makedirs(os.path.join(esp, "EFI", "BOOT"))
    shutil.copy(LOADER, os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI"))
    shutil.copy(KERNEL64, os.path.join(esp, "threados64.bin"))
    shutil.copy(INITRD64, os.path.join(esp, "initrd64.img"))
    os.makedirs(os.path.join(esp, "apps"))
    shutil.copy(apps, os.path.join(esp, "apps", PKG))
    stick_starts_live(esp)       # this group types at the shell


def fat_free(path, offset):
    """What mtools says is free on the filesystem, in bytes"""
    r = subprocess.run(["mdir", "-i", _at(path, offset), "::"],
                       env=MTOOLS_ENV, capture_output=True)
    m = re.search(r"([\d ]+) bytes free", r.stdout.decode(errors="replace"))
    return int(m.group(1).replace(" ", "")) if m else 0


def boot_stick(esp, disk, slog, nvram):
    shutil.copy(OVMF_VARS, nvram)
    q = Qemu(KERNEL, INITRD, esp=esp, disk=disk, sata=True, serial=slog,
             qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
    q.wait_serial("The 64-bit system is ready", timeout=120)
    return q


def beside(work, esp, apps, esp_mib, fill_to=None):
    """Installing beside a foreign system; 'fill_to' leaves that many
    bytes free on its EFI partition. The installer's output and the
    disk."""
    target = os.path.join(work, "beside%d.img" % esp_mib)
    make_gpt_disk(target, mib=esp_mib + 400, esp_mib=esp_mib)
    fat_format(target, ESP_START, esp_mib * 2048, fat32=True, label="OLD")
    if fill_to is not None:
        free = fat_free(target, ESP_START)
        if free > fill_to:
            fat_put(target, ESP_START, b"\0" * (free - fill_to),
                    "/somebody.bin")
    q = boot_stick(esp, target, os.path.join(work, "b%d.log" % esp_mib),
                   os.path.join(work, "vars.fd"))
    try:
        check("the stick's system reaches its desktop", q.wait_desktop())
        log = q.serial()
        m = re.search(r"Package probe 1: Probe, (\d+) KiB", log)
        check("the stick's loader brought the package from \\apps",
              m is not None
              and int(m.group(1)) == os.path.getsize(apps) // 1024)
        plan = q.command("install text", "Type YES", timeout=40)
        done = q.command("YES", "Done", timeout=180)
    finally:
        q.close()
    return target, plan, done


HELLO = {}
GT_W = _kernel_define("user/gtapp.c", "W0")
GT_H = _kernel_define("user/gtapp.c", "H0")
GT_ROW_H = _kernel_define("user/gtapp.c", "ROW_H")
GT_ROW_Y0 = _kernel_define("user/gtapp.c", "ROW_Y0")
GT_BTN_W = _kernel_define("user/gtapp.c", "BTN_W")
GT_BTN_H = _kernel_define("user/gtapp.c", "BTN_H")
GT_GAP = _kernel_define("user/gtapp.c", "BTN_GAP")


def installed_from_files(q, port):
    """hello.tapp on the Desktop, double-clicked: the desktop has the
    Files app open it (gfiles --open-index), which opens a package in
    the window that installs it"""
    q.command("run httpget 10.0.2.2 %d /hello.tapp Desktop/hello.tapp"
              % port, "[httpget] done", timeout=60)
    # The first file on the Desktop, after the three shortcuts, where a
    # desktop nobody rearranged lays it out (t_desktop64)
    time.sleep(4)
    q.mouse_goto(10 + 3 * 88 + 38, BAR_H + 8 + 34)
    time.sleep(0.4)
    q.double_click()
    # Out of the way: the window opens where the pointer is, and a
    # pointer over its title is a title nobody can read
    sw, sh = q.size()
    time.sleep(0.5)
    q.mouse_goto(sw - 4, sh - 4)
    where = None
    end = time.time() + 40
    while time.time() < end and where is None:
        time.sleep(0.5)
        for slot in range(8):
            x, y = cascade(slot, GT_W + 2, GT_H + TITLE_H + 2, w=sw, h=sh)
            if q.window_title(x, y, GT_W + 2) == "Package":
                where = (x + 1, y + CONTENT_DY)
                break
    check("a double click on the package on the Desktop opens the window "
          "that installs it", where is not None)
    if where is None:
        return
    cx, cy = where

    def rows(n=11):
        return q.read_ui_lines(cx + 10, cx + GT_W - 4,
                               [cy + ui_font_baseline_in(
                                   UD_PX, GT_ROW_Y0 + r * GT_ROW_H, GT_ROW_H)
                                for r in range(n)], px=UD_PX)

    q.mouse_goto(sw - 4, sh - 4)
    time.sleep(1)
    text = rows()
    check(f"it says what the package is and that the system's key signed "
          f"it ({text!r})",
          "Hello, version 1" in text and "hello - runs tapphi" in text and
          "Signed by the system's key" in text and
          "It goes into /EFI/ThreadOS/apps" in text)
    span = 2 * GT_BTN_W + GT_GAP
    install = (cx + (GT_W - span) // 2 + GT_BTN_W // 2,
               cy + GT_H - GT_BTN_H - 12 + GT_BTN_H // 2)
    # The shell behind an administrator's desktop is the administrator's,
    # and so is what it starts: no password is asked here (t_tapp asks
    # for one, in a session that is not an administrator's)
    seen = len(q.serial())
    q.click(*install)
    end = time.time() + 60
    text = rows()
    while time.time() < end and "Installed" not in text:
        time.sleep(1)
        text = rows()
    check(f"Install installs it ({text!r})",
          "Installed - it runs now" in text and
          "Package hello 1 installed" in q.serial()[seen:])
    # Close, and the keys back to the shell
    q.click(cx + GT_W // 2, cy + GT_H - GT_BTN_H - 12 + GT_BTN_H // 2)
    end = time.time() + 20
    while time.time() < end and "'gtapp' has ended" not in q.serial()[seen:]:
        time.sleep(0.5)
    q.click_desktop()
    time.sleep(1)


def installed_while_running(q, work):
    """'tapp install' on the installed system: a package from the
    network, signed with the test's key"""
    key = os.path.join(work, "key.pem")
    subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey",
                    "-noout", "-out", key], check=True, capture_output=True)
    pub = subprocess.run(["openssl", "ec", "-in", key, "-pubout", "-outform",
                          "DER"], check=True,
                         capture_output=True).stdout[-65:].hex()
    mk = os.path.join(ROOT, "scripts", "mktapp.py")
    hello, plain = (os.path.join(work, n) for n in ("hello.tapp",
                                                    "plain.tapp"))
    subprocess.run([sys.executable, mk, hello, "--id", "hello", "--name",
                    "Hello", "--version", "1", "--arch", "x86_64", "--exec",
                    "tapphi", "--dock", "--key", key, "tapphi=" + ECHO],
                   check=True, capture_output=True)
    subprocess.run([sys.executable, mk, plain, "--id", "plain", "--name",
                    "Plain", "--version", "1", "--arch", "x86_64", "--exec",
                    "tappplain", "tappplain=" + ECHO],
                   check=True, capture_output=True)
    HELLO["data"] = open(hello, "rb").read()
    SERVED["/hello.tapp"] = HELLO["data"]
    SERVED["/plain.tapp"] = open(plain, "rb").read()
    SERVED["/update.key"] = pub.encode() + b"\n"
    port, stop = start_http_server()
    try:
        q.wait_serial("[net] DHCP", timeout=60)
        for name in ("hello.tapp", "plain.tapp"):
            q.command("run httpget 10.0.2.2 %d /%s %s" % (port, name, name),
                      "[httpget] done", timeout=60)
        out = q.sudo("run tapp install hello.tapp", "[tapp] done",
                     timeout=120)
        check("a package signed by a key the system does not trust is "
              "not installed",
              "hello.tapp: NOT signed by the system's key" in out and
              "not installed" in out)
        q.sudo("run httpget 10.0.2.2 %d /update.key /update.key" % port,
               "[httpget] done", timeout=60)
        out = q.sudo("run tapp install plain.tapp", "[tapp] done",
                     timeout=120)
        check("an unsigned one never is",
              "plain.tapp: has no signature" in out and
              "not installed" in out)
        seen = len(q.serial())
        installed_from_files(q, port)
        check("one signed by the key in update.key is: written, and its "
              "programs run now",
              "Package hello 1 installed: Hello" in q.serial()[seen:])
        end = time.time() + 20
        while time.time() < end and "a tile for the package hello" not in \
                q.serial()[seen:]:
            time.sleep(0.5)
        check("its tile comes into the dock",
              "[desktop] a tile for the package hello: Hello"
              in q.serial()[seen:])
        out = q.command("run tapphi installed-just-now", "exit code",
                        timeout=60)
        check("and its program runs",
              "installed-just-now" in out and
              "[tapphi] done, exit code 0" in out)
        if "[tapphi] done" not in out:
            print("   | " + "\n   | ".join(
                q.serial()[-1500:].replace("\r", "").split("\n")))
        out = q.sudo("run tapp install hello.tapp", "[tapp] done",
                     timeout=120)
        check("the same version a second time is refused",
              "that version or a newer one is installed already" in out)
    finally:
        stop()


def main():
    print()
    print("--- installing the application packages ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    if not mtools_ok():
        print("SKIPPED: no mtools")
        return summary()
    if not os.path.exists(LOADER):
        subprocess.run(["make", "BUILD=" + BUILD, "efi"], cwd=ROOT,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not os.path.exists(LOADER):
        print("SKIPPED: no build/BOOTX64.EFI (apt install clang lld)")
        return summary()
    if not os.path.exists(OVMF_CODE) or not os.path.exists(OVMF_VARS):
        print("SKIPPED: no OVMF firmware")
        return summary()
    if not os.path.exists(ECHO):
        print("SKIPPED: no uecho64 - make")
        return summary()

    work = tempfile.mkdtemp()
    try:
        apps = os.path.join(work, PKG)
        check("a package with a program in it", make_apps(apps))
        want_apps = blob(apps)
        want_initrd = blob(INITRD64)
        esp = os.path.join(work, "stick")
        make_stick(esp, apps)

        # --- beside, with room: the package goes into /EFI/ThreadOS/apps
        target, plan, done = beside(work, esp, apps, 600)
        check("the plan lists the package with its size",
              "%s  %d B\n" % (PKG_PATH, len(want_apps))
              in plan.replace("\r", "") and "DOES NOT FIT" not in plan)
        check("and writes it after the initrd",
              "/EFI/ThreadOS/initrd64.img - done" in done
              and "/EFI/ThreadOS/apps - done" in done
              and done.index("initrd64.img - done")
              < done.index("/EFI/ThreadOS/apps - done"))
        got = fat_get(target, ESP_START, PKG_PATH)
        check("mtools reads the package off the disk byte for byte (%d B)"
              % len(want_apps), got == want_apps)

        # --- beside, with too little room: the system without it.
        # Room for the system once and a half, which is enough to
        # install and not enough to keep an update's copy beside it
        # with the package in the way.
        need = (2 * os.path.getsize(LOADER) + os.path.getsize(KERNEL64)
                + len(want_initrd)) + 64 * 1024
        target, plan, done = beside(work, esp, apps, 300,
                                    fill_to=need * 3 // 2)
        check("on a nearly full EFI partition the plan says the package "
              "does not fit, before anything is written",
              "%s  %d B  DOES NOT FIT" % (PKG_PATH, len(want_apps))
              in plan and "too little room for them" in plan)
        check("the system is installed all the same",
              "/EFI/ThreadOS/initrd64.img - done" in done
              and "boot menu entry (UEFI) - done" in done)
        check("without the package", "/EFI/ThreadOS/apps - done" not in done
              and fat_get(target, ESP_START, PKG_PATH) is None)
        check("and with the initrd whole",
              fat_get(target, ESP_START, "/EFI/ThreadOS/initrd64.img")
              == want_initrd)

        # --- the whole disk: an EFI partition that holds it
        target = os.path.join(work, "whole.img")
        with open(target, "wb") as fh:
            fh.truncate((ESP_MIB_APPS + 200) << 20)
        nvram = os.path.join(work, "whole.fd")
        q = boot_stick(esp, target, os.path.join(work, "w.log"), nvram)
        try:
            check("the stick's system reaches its desktop", q.wait_desktop())
            plan = q.command("install text whole", "Type ERASE", timeout=40)
            check("the plan makes an EFI partition of %d MiB for the package"
                  % ESP_MIB_APPS,
                  "an EFI partition of %d MiB (FAT32)" % ESP_MIB_APPS in plan
                  and "%s  %d B" % (PKG_PATH, len(want_apps)) in plan)
            done = q.command("ERASE", "Done", timeout=240)
            check("and writes the package onto it",
                  "/EFI/ThreadOS/apps - done" in done)
        finally:
            q.close()
        ok, parts, why = gpt_read(target)
        check("the table holds together and its EFI partition is %d MiB"
              % ESP_MIB_APPS,
              ok and len(parts) == 2
              and parts[0][2] - parts[0][1] + 1 == ESP_MIB_APPS * 2048)
        got = fat_get(target, ESP_START, PKG_PATH)
        check("mtools reads the package off it byte for byte",
              got == want_apps)

        # --- and the installed system, on its own, uses it
        q = Qemu(KERNEL, INITRD, disk=target, sata=True,
                 serial=os.path.join(work, "w2.log"),
                 qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            out = q.wait_serial("The 64-bit system is ready", timeout=120)
            m = re.search(r"Package probe 1: Probe, (\d+) KiB", out)
            check("the installed loader hands the package over from its "
                  "own disk (%s)" % (m.group(0) if m else "nothing said"),
                  m is not None
                  and int(m.group(1)) == len(want_apps) // 1024)
            check("the installed system reaches its desktop",
                  q.wait_desktop())
            out = q.command("run %s off-the-installed-disk" % PROBE,
                            "exit code", timeout=60)
            check("and runs a program out of it",
                  "off-the-installed-disk" in out
                  and "[%s] done, exit code 0" % PROBE in out)
            installed_while_running(q, work)
        finally:
            q.close()
        got = fat_get(target, ESP_START, "/EFI/ThreadOS/apps/hello.tapp")
        check("the package installed while the system ran is on its disk, "
              "byte for byte", got == HELLO["data"])
        q = Qemu(KERNEL, INITRD, disk=target, sata=True,
                 serial=os.path.join(work, "w3.log"),
                 qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            out = q.wait_serial("The 64-bit system is ready", timeout=120)
            check("and the next start takes it from there",
                  "Package hello 1: Hello" in out
                  and "Package probe 1: Probe" in out)
        finally:
            q.close()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
