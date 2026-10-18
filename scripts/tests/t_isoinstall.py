#!/usr/bin/env python3
"""ThreadOS smoke test: installing from the disc (threados64.iso).

The stick image (make stick) starts through threadboot, which hands the
installer the files it writes. The disc starts through GRUB - and
people put a disc image on a stick with whatever program they have
(Rufus, Etcher, Ventoy, dd) rather than with ours. So the disc has to
be able to install too: it carries threadboot as a plain file, and
every one of its menu entries has GRUB hand over the kernel's file and
threadboot as modules, by the names threadboot itself gives them.

Checked from outside, on the disc 'make iso64' really builds:

  its GRUB menu is the one in boot/grub64.cfg, and threadboot is on it byte
  for byte

  started under UEFI firmware (OVMF), with somebody else's disk behind
  an AHCI controller, 'install text' writes the three files - read back
  off the image by mtools and compared with the build's own - and
  leaves the other system's loader alone

  that disk then boots on its own, through threadboot, with no disc

  and the menu's install entry starts the installer by itself

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
from ptest import (check, summary, no64, make_gpt_disk, gpt_intact,
                   mtools_ok, fat_format, fat_mkdirs, fat_put, fat_get)

ESP_START = 2048
ESP_SECTORS = 204800
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
ISO = os.path.join(ROOT, "build", "threados64.iso")
LOADER = os.path.join(ROOT, "build", "BOOTX64.EFI")
FOREIGN_BOOTMGR = b"MZ-this-is-somebody-elses-bootmgfw-" * 300


def blob(path):
    with open(path, "rb") as fh:
        return fh.read()


def iso_file(iso, path, out):
    """A file off the disc, by xorriso - somebody else's reading of it"""
    r = subprocess.run(["xorriso", "-osirrox", "on", "-indev", iso,
                        "-extract", path, out],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return blob(out) if r.returncode == 0 and os.path.exists(out) else None


def remaster(iso, work, default):
    """The same disc with another menu entry chosen and no wait: the
    tree off the disc, its grub.cfg with only 'default' changed"""
    tree = os.path.join(work, "tree%d" % default)
    subprocess.run(["xorriso", "-osirrox", "on", "-indev", iso,
                    "-extract", "/boot", os.path.join(tree, "boot")],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    cfg = os.path.join(tree, "boot", "grub", "grub.cfg")
    os.chmod(cfg, 0o644)
    text = blob(cfg).decode()
    text = text.replace("set default=0", "set default=%d" % default)
    text = text.replace("set timeout=2", "set timeout=0")
    with open(cfg, "w") as fh:
        fh.write(text)
    for root, dirs, files in os.walk(tree):
        for n in dirs + files:
            os.chmod(os.path.join(root, n), 0o755)
    out = os.path.join(work, "menu%d.iso" % default)
    r = subprocess.run(["grub-mkrescue", "-o", out, tree],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return out if r.returncode == 0 else None


def make_target(path, taken=False):
    """The other machine's disk: a GPT, an EFI partition, Windows' loader
    - and with 'taken', Windows' copy under the fallback name too, which
    is how a laptop sold with Windows comes"""
    make_gpt_disk(path)
    if not fat_format(path, ESP_START, ESP_SECTORS, fat32=True,
                      label="LAPTOPESP"):
        return False
    fat_mkdirs(path, ESP_START, ["/EFI", "/EFI/Microsoft",
                                 "/EFI/Microsoft/Boot"])
    fat_put(path, ESP_START, FOREIGN_BOOTMGR,
            "/EFI/Microsoft/Boot/bootmgfw.efi")
    if taken:
        fat_mkdirs(path, ESP_START, ["/EFI/BOOT"])
        fat_put(path, ESP_START, FOREIGN_BOOTMGR, "/EFI/BOOT/BOOTX64.EFI")
    return True


def main():
    print()
    print("--- installing from the disc ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    for tool in ("xorriso", "grub-mkrescue"):
        if shutil.which(tool) is None:
            print("SKIPPED: no " + tool)
            return summary()
    if not mtools_ok():
        print("SKIPPED: no mtools")
        return summary()
    if not os.path.exists(OVMF_CODE) or not os.path.exists(OVMF_VARS):
        print("SKIPPED: no OVMF firmware")
        return summary()
    if not os.path.exists(ISO) or not os.path.exists(LOADER):
        print("SKIPPED: no build/threados64.iso or build/BOOTX64.EFI "
              "(make iso64 efi)")
        return summary()

    work = tempfile.mkdtemp()
    target = os.path.join(work, "target.img")
    nvram = os.path.join(work, "vars.fd")
    want_loader = blob(LOADER)
    want_kernel = blob(KERNEL64)
    want_initrd = blob(INITRD64)
    try:
        # --- The disc itself
        cfg = iso_file(ISO, "/boot/grub/grub.cfg", os.path.join(work, "g.cfg"))
        check("the disc's GRUB menu is boot/grub64.cfg",
              cfg is not None and cfg == blob(os.path.join(ROOT, "boot", "grub64.cfg")))
        entries = re.findall(r'menuentry "([^"]+)" \{(.*?)\}',
                             (cfg or b"").decode(), re.S)
        systems = [(n, b) for n, b in entries if "multiboot2" in b]
        check(f"every one of its {len(systems)} entries that start the "
              "system hands over the kernel's file and threadboot, by the "
              "names the installer looks for",
              len(systems) == 3 and all(
                  "module2 /boot/threados64.bin the kernel" in body
                  and "module2 /boot/threadboot.efi loader" in body
                  for _, body in systems))
        register = [i for i, (n, b) in enumerate(entries)
                    if "chainloader /boot/threadboot.efi register" in b]
        check("and one more starts threadboot to add the installed system "
              "to the firmware's boot menu", len(register) == 1)
        check("one of them is the installer",
              any(" install\n" in body for _, body in entries))
        got = iso_file(ISO, "/boot/threadboot.efi", os.path.join(work, "l.efi"))
        check(f"threadboot is on the disc byte for byte ({len(want_loader)} B)",
              got == want_loader)

        shell_iso = remaster(ISO, work, 2)
        install_iso = remaster(ISO, work, 1)
        register_iso = remaster(ISO, work, register[0]) if register else None
        check("(the same disc, with the shell, the installer or the boot "
              "menu entry chosen)",
              shell_iso is not None and install_iso is not None
              and register_iso is not None)
        if not shell_iso or not install_iso or not register_iso:
            return summary()

        # --- Installing in text, from the disc
        check("somebody else's disk is ready (GPT + FAT32 from mtools)",
              make_target(target))
        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, cdrom=shell_iso, disk=target, sata=True,
                 serial=os.path.join(work, "s1.log"),
                 qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            q.wait_serial("The 64-bit system is ready", timeout=120)
            out = q.command("install text", "Type YES", timeout=30)
            check("the installer started from GRUB has something to install "
                  "and says what it would write",
                  "/EFI/ThreadOS/threados64.bin" in out
                  and "nothing to install" not in out)
            out = q.command("YES", "Done", timeout=90)
            check("after YES it writes the three files and the fallback",
                  all(f"{p} - done" in out for p in (
                      "/EFI/ThreadOS/BOOTX64.EFI",
                      "/EFI/ThreadOS/threados64.bin",
                      "/EFI/ThreadOS/initrd64.img",
                      "/EFI/BOOT/BOOTX64.EFI")))
        finally:
            q.close()

        for path, want in (("/EFI/ThreadOS/BOOTX64.EFI", want_loader),
                           ("/EFI/ThreadOS/threados64.bin", want_kernel),
                           ("/EFI/ThreadOS/initrd64.img", want_initrd),
                           ("/EFI/BOOT/BOOTX64.EFI", want_loader)):
            got = fat_get(target, ESP_START, path)
            check(f"{path} byte for byte, as mtools reads it ({len(want)} B)",
                  got is not None and got == want)
        check("Windows' loader is untouched",
              fat_get(target, ESP_START, "/EFI/Microsoft/Boot/bootmgfw.efi")
              == FOREIGN_BOOTMGR)
        check("and so is the partition table", gpt_intact(target))

        # --- The disk on its own, with no disc
        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, disk=target, sata=True,
                 serial=os.path.join(work, "s2.log"),
                 qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            out = q.wait_serial("threadboot: ThreadOS", timeout=60)
            check("the installed disk boots on its own, through threadboot",
                  "threadboot" in out)
            out = q.wait_serial("The 64-bit system is ready", timeout=90)
            check("and reaches the system", "Multiboot OK" in out)
        finally:
            q.close()

        # --- The menu's own install entry
        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, cdrom=install_iso,
                 disk=os.path.join(work, "target.img"), sata=True,
                 serial=os.path.join(work, "s3.log"),
                 qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            q.wait_serial("The installer is starting", timeout=120)
            time.sleep(10)
            q.mouse_goto(900, 600)
            time.sleep(0.8)
            # The installer's own screen, read the way t_ginstal reads
            # it: no menu bar over the aurora, and its heading
            w, h, at = q.screen_raw()
            top = sum(1 for x in range(0, w, 8)
                      if at(x, 3) == aurora_px(x, 3, w, h))
            head = q.read_ui(14, ui_font_baseline_in(BAR_PX + 2, 8, 20),
                             width=260, px=BAR_PX + 2, face="bold",
                             fg=(255, 255, 255),
                             under=lambda x, y: aurora_px(x, y, w, h))
            check("the install entry starts the installer by itself, on the "
                  f"whole screen ('{head}', bare aurora {top}/{w // 8} "
                  "along the top)",
                  "ThreadOS - installation" in head and top == w // 8)
        finally:
            q.close()
        boot_menu(work, shell_iso, register_iso, nvram)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


def boot_menu(work, shell_iso, register_iso, nvram):
    """A laptop sold with Windows: the fallback name is Windows', so the
    installer leaves it - and tells the firmware about ThreadOS itself,
    through the UEFI runtime services (kernel64/efirt.c)"""
    laptop = os.path.join(work, "laptop.img")
    check("a laptop's disk, with Windows under the fallback name too",
          make_target(laptop, taken=True))
    shutil.copy(OVMF_VARS, nvram)
    q = Qemu(KERNEL, INITRD, cdrom=shell_iso, disk=laptop, sata=True,
             serial=os.path.join(work, "s4.log"),
             qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
    try:
        q.wait_serial("The 64-bit system is ready", timeout=120)
        q.command("install text", "Type YES", timeout=30)
        out = q.command("YES", "Done", timeout=90)
        made = re.search(r"boot menu as Boot([0-9A-F]{4}) \(last", out)
        check("the installer adds ThreadOS to the firmware's boot menu "
              "itself - last, since the machine has a system of its own "
              f"({'Boot' + made.group(1) if made else 'no entry'})",
              made is not None and "boot menu entry (UEFI) - done" in out)
    finally:
        q.close()
    if not made:
        return
    number = made.group(1)
    check("Windows' fallback loader is still Windows'",
          fat_get(laptop, ESP_START, "/EFI/BOOT/BOOTX64.EFI")
          == FOREIGN_BOOTMGR)

    # The disc's entry, on the same firmware (the same variables)
    q = Qemu(KERNEL, INITRD, cdrom=register_iso, disk=laptop, sata=True,
             serial=os.path.join(work, "s5.log"),
             qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
    try:
        out = q.wait_serial("to the boot menu", timeout=120)
        m = re.search(r"added (Boot[0-9A-F]{4}) \"ThreadOS\"", out)
        check("the disc's own entry for it finds the entry the installer "
              "made and rewrites it rather than adding a second "
              f"({m.group(1) if m else 'none'})",
              m is not None and m.group(1) == "Boot" + number)
    finally:
        q.close()

    # And the laptop with no disc at all: Windows' loader (here a file
    # that is no program) fails, and the firmware goes down its list to
    # the entry threadboot added. (No network card: OVMF would try to
    # boot off the network first, and give up on that only slowly.)
    q = Qemu(KERNEL, INITRD, disk=laptop, sata=True, nic=None,
             serial=os.path.join(work, "s6.log"),
             qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
    try:
        # OVMF keeps its own UEFI shell ahead of anything added later,
        # and that is where it stops. Its 'bcfg' reads the boot entries
        # back - somebody else's reading of what threadboot wrote - and
        # BootNext is what a laptop's F12 menu sets when ThreadOS is
        # picked from it: boot this entry, once.
        q.wait_serial("Shell> ", timeout=120)
        time.sleep(1)
        seen = len(q.serial())
        q.type_line("bcfg boot dump")
        time.sleep(4)
        dump = q.serial()[seen:]
        flat = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", dump)
        ours = re.search(r"Boot%s\s+Desc\s+- ThreadOS\s+DevPath\s+- (\S+)"
                         % number, flat)
        check("the firmware's own shell lists the entry - ThreadOS, the "
              "partition by its GUID and the file on it "
              f"({ours.group(1) if ours else 'not there'})",
              ours is not None and ours.group(1).startswith("HD(1,GPT,")
              and ours.group(1).endswith("\\EFI\\ThreadOS\\BOOTX64.EFI"))
        q.type_line("setvar BootNext -guid 8be4df61-93ca-11d2-aa0d-00e098032b8c "
                    f"-bs -rt -nv ={number[2:]}{number[:2]}")
        time.sleep(3)
    finally:
        q.close()

    # The next start: the variables are flash, kept in the file
    q = Qemu(KERNEL, INITRD, disk=laptop, sata=True, nic=None,
             serial=os.path.join(work, "s7.log"),
             qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
    try:
        deadline = time.time() + 150
        while (time.time() < deadline
               and "The 64-bit system is ready" not in q.serial()):
            time.sleep(1)
        out = q.serial()
        ok = ("threadboot: ThreadOS" in out and "Multiboot OK" in out
              and "The 64-bit system is ready" in out)
        check("with no disc, the firmware starts ThreadOS from that entry"
              + ("" if ok else f" (the port's last words: {out[-300:]!r})"),
              ok)
    finally:
        q.close()


sys.exit(main())
