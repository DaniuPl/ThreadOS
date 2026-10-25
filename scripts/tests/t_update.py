#!/usr/bin/env python3
"""ThreadOS smoke test: updating an installed system over the network.

A release is update.txt (version, sizes, SHA-256s, an ECDSA P-256
signature) and three files next to it; 'update' (user/update.c) fetches
it, checks the signature and every hash, and the kernel writes the
files over the installed ones (SYS64_UPDATE_WRITE). The release
workflow publishes them on GitHub; here a server on this machine plays
GitHub, with a key of the test's own, which the system is told to
trust through update.key (a system file: only an admin writes it).

From outside:

  a machine installs ThreadOS onto its whole disk from a stick, and
  starts from that disk; its update sequence is 0, a build by hand

  'update check' sees release 7 and installs nothing; 'update' without
  sudo is refused; with sudo it downloads, checks and writes - and
  mtools reads the new kernel, initrd and loader off the disk image
  byte for byte; the previous files are kept so an admin can roll back

  a separate GRUB Live boot sees a valid signed release but the kernel
  refuses to write it to the installed disk

  restarted, the disk is update sequence 7, and up to date

  a release signed by some other key, and one whose initrd does not
  match its hash, are both refused - and the disk is left as it was

  'sudo run update rollback' restores the original kernel, initrd and
  loader byte for byte, and that restored system boots again

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, mtools_ok, fat_get

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOADER = os.path.join(os.path.dirname(os.path.abspath(KERNEL64 or "build/x")), "BOOTX64.EFI")
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
ESP_START = 2048
DISK_MIB = 300


def initrd_with_version(src, version):
    """The same initrd with its monotonic update sequence changed."""
    raw = open(src, "rb").read()
    count = struct.unpack_from("<I", raw, 4)[0]
    files = []
    for i in range(count):
        name, off, size = struct.unpack_from("<16sII", raw, 8 + 24 * i)
        files.append((name.rstrip(b"\0"), raw[off:off + size]))
    has_code = any(n == b"version-code" for n, _ in files)
    if has_code:
        files = [
            (n, (str(version) + "\n").encode() if n == b"version-code" else
             (("0.1.0-test.%d\n" % version).encode()
              if n == b"version" else d))
            for n, d in files
        ]
    else:
        files = [(n, (str(version) + "\n").encode() if n == b"version" else d)
                 for n, d in files]
    out = bytearray(b"TIRD" + struct.pack("<I", len(files)))
    offset = 8 + 24 * len(files)
    for n, d in files:
        out += struct.pack("<16sII", n, offset, len(d))
        offset += len(d)
    for _, d in files:
        out += d
    return bytes(out)


def make_key(path):
    subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey",
                    "-noout", "-out", path], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    der = subprocess.run(["openssl", "ec", "-in", path, "-pubout", "-outform",
                          "DER"], capture_output=True, check=True).stdout
    return der[-65:].hex()


def release(work, key, version, kernel, initrd, loader, name):
    """update.txt for these three files, signed with 'key'"""
    paths = []
    for n, data in (("k", kernel), ("i", initrd), ("l", loader)):
        p = os.path.join(work, name + n)
        with open(p, "wb") as fh:
            fh.write(data)
        paths.append(p)
    out = os.path.join(work, name + ".txt")
    subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "mkupdate.py"),
                    out, str(version), key] + paths, check=True,
                   stdout=subprocess.DEVNULL)
    return open(out, "rb").read()


def serve(prefix, manifest, kernel, initrd, loader):
    SERVED[prefix + "update.txt"] = manifest
    SERVED[prefix + "threados64.bin"] = kernel
    SERVED[prefix + "initrd64.img"] = initrd
    SERVED[prefix + "BOOTX64.EFI"] = loader


def boot_disk(disk, nvram, serial):
    return Qemu(KERNEL, INITRD, disk=disk, sata=True, serial=serial,
                qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))


def make_live_iso(work, kernel, initrd):
    root = os.path.join(work, "live-iso")
    os.makedirs(os.path.join(root, "boot", "grub"))
    shutil.copy(kernel, os.path.join(root, "boot", "threados64.bin"))
    shutil.copy(initrd, os.path.join(root, "boot", "initrd64.img"))
    with open(os.path.join(root, "boot", "grub", "grub.cfg"), "w") as fh:
        fh.write("set timeout=0\nset default=0\n"
                 'menuentry "ThreadOS Live" {\n'
                 "    multiboot2 /boot/threados64.bin live\n"
                 "    module2 /boot/initrd64.img initrd64\n}\n")
    iso = os.path.join(work, "live.iso")
    subprocess.run(["grub-mkrescue", "-o", iso, root], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return iso


def after(q, seen, text, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline and text not in q.serial()[seen:]:
        time.sleep(0.5)
    return q.serial()[seen:]


def run(q, line, end="[update] done", timeout=240, password=False):
    seen = len(q.serial())
    q.type_line(line)
    if password:
        after(q, seen, "password:", 30)
        q.type_line("thread")
    return after(q, seen, end, timeout)


def main():
    print()
    print("--- updating an installed system ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    if not mtools_ok() or not os.path.exists(LOADER) or \
            not os.path.exists(OVMF_CODE) or shutil.which("openssl") is None or \
            shutil.which("grub-mkrescue") is None:
        print("SKIPPED: needs mtools, build/BOOTX64.EFI, OVMF, openssl "
              "and grub-mkrescue")
        return summary()

    work = tempfile.mkdtemp()
    port, stop = start_http_server()
    try:
        kernel = open(KERNEL64, "rb").read()
        loader = open(LOADER, "rb").read()
        initrd7 = initrd_with_version(INITRD64, 7)
        initrd0 = initrd_with_version(INITRD64, 0)
        initrd9 = initrd_with_version(INITRD64, 9)
        good_key = os.path.join(work, "good.pem")
        other_key = os.path.join(work, "other.pem")
        pub = make_key(good_key)
        make_key(other_key)
        SERVED["/update.key"] = pub.encode() + b"\n"
        serve("/rel/", release(work, good_key, 7, kernel, initrd7, loader,
                               "rel"), kernel, initrd7, loader)
        serve("/forged/", release(work, other_key, 9, kernel, initrd7, loader,
                                  "forged"), kernel, initrd7, loader)
        serve("/live/", release(work, good_key, 9, kernel, initrd9, loader,
                                "live"), kernel, initrd9, loader)
        broken = bytearray(initrd7)
        broken[len(broken) // 2] ^= 0xFF
        serve("/broken/", release(work, good_key, 9, kernel, initrd7, loader,
                                  "broken"), kernel, bytes(broken), loader)
        base = "http://10.0.2.2:%d/" % port

        # --- a machine with ThreadOS on its whole disk
        stick = os.path.join(work, "stick")
        os.makedirs(os.path.join(stick, "EFI", "BOOT"))
        shutil.copy(LOADER, os.path.join(stick, "EFI", "BOOT", "BOOTX64.EFI"))
        shutil.copy(KERNEL64, os.path.join(stick, "threados64.bin"))
        shutil.copy(INITRD64, os.path.join(stick, "initrd64.img"))
        disk = os.path.join(work, "disk.img")
        with open(disk, "wb") as fh:
            fh.truncate(DISK_MIB * 1024 * 1024)
        nvram = os.path.join(work, "vars.fd")
        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, esp=stick, disk=disk, sata=True,
                 serial=os.path.join(work, "s1.log"),
                 qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            q.wait_serial("The 64-bit system is ready", timeout=120)
            q.command("install text whole", "Type ERASE", timeout=40)
            out = q.command("ERASE", "Done", timeout=180)
            check("ThreadOS installs onto the whole disk from a stick",
                  "/EFI/ThreadOS/threados64.bin - done" in out)
        finally:
            q.close()

        installed_paths = (
            "/EFI/ThreadOS/threados64.bin",
            "/EFI/ThreadOS/initrd64.img",
            "/EFI/ThreadOS/BOOTX64.EFI",
            "/EFI/BOOT/BOOTX64.EFI",
        )
        old_files = {path: fat_get(disk, ESP_START, path)
                     for path in installed_paths}
        check("the freshly installed system files are present for rollback",
              all(data is not None for data in old_files.values()))

        q = boot_disk(disk, nvram, os.path.join(work, "s2.log"))
        try:
            q.wait_serial("The 64-bit system is ready", timeout=120)
            q.wait_serial("[net] DHCP", timeout=60)

            out = run(q, "run update check " + base + "rel/",
                      end="[update] done, exit code")
            check("the installed system shows a pre-1.0 label and sequence 0",
                  "this system is version 0.1.0-" in out and
                  "(update sequence 0)" in out)
            check("'update check' sees the signed release 7 and installs "
                  "nothing - with the built-in key it is refused",
                  "NOT signed by the right key" in out)

            # The test's key, the way an admin would put it there
            out = run(q, "sudo run httpget 10.0.2.2 %d /update.key update.key"
                      % port, end="[httpget] done", password=True)
            check("update.key goes on the disk (a system file, through sudo)",
                  "[httpget] done, exit code 0" in out)

            out = run(q, "run update check " + base + "rel/",
                      end="[update] done, exit code")
            check("now release 7 checks out, and 'check' only reports it",
                  "signed by the right key" in out and
                  "newest release update sequence is 7" in out and
                  "a newer system is available" in out and
                  "written:" not in out)

            out = run(q, "run update " + base + "rel/",
                      end="[update] done, exit code")
            check("without sudo the system is not replaced",
                  "only an admin may replace the system" in out)

            out = run(q, "sudo run update " + base + "rel/",
                      end="[update] done, exit code", password=True)
            check("with sudo: downloaded, every hash checked, written",
                  "all three files match the signed hashes" in out and
                  out.count("[update] written:") == 3 and
                  "updated to release sequence 7" in out and
                  "[update] done, exit code 0" in out)
        finally:
            q.close()

        for path, want in (("/EFI/ThreadOS/threados64.bin", kernel),
                           ("/EFI/ThreadOS/initrd64.img", initrd7),
                           ("/EFI/ThreadOS/BOOTX64.EFI", loader),
                           ("/EFI/BOOT/BOOTX64.EFI", loader)):
            got = fat_get(disk, ESP_START, path)
            check(f"{path} on the disk is the release's, byte for byte",
                  got == want)

        live_iso = make_live_iso(work, KERNEL64, initrd0)
        q = Qemu(KERNEL, INITRD, cdrom=live_iso, disk=disk, sata=True,
                 serial=os.path.join(work, "live.log"),
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=60)
            q.wait_serial("[net] DHCP", timeout=60)
            out = run(q, "sudo run update " + base + "live/",
                      end="[update] done, exit code", password=True)
            check("a valid signed update is refused from a Live boot",
                  "all three files match the signed hashes" in out and
                  "updates can only be installed after booting from an "
                  "installed system" in out and
                  "[update] written:" not in out)
        finally:
            q.close()

        q = boot_disk(disk, nvram, os.path.join(work, "s3.log"))
        try:
            q.wait_serial("The 64-bit system is ready", timeout=120)
            q.wait_serial("[net] DHCP", timeout=60)
            out = run(q, "run update check " + base + "rel/",
                      end="[update] done, exit code")
            check("restarted, the disk is update sequence 7 - and up to date",
                  "this system is version 0.1.0-test.7" in out and
                  "(update sequence 7)" in out and
                  "this system is up to date" in out)

            out = run(q, "sudo run update " + base + "forged/",
                      end="[update] done, exit code", password=True)
            check("a release signed by another key is refused",
                  "NOT signed by the right key" in out and
                  "written:" not in out)
            out = run(q, "sudo run update " + base + "broken/",
                      end="[update] done, exit code", password=True)
            check("a release whose initrd does not match its hash is refused",
                  "does not match its hash" in out and "written:" not in out)

            check("refused releases left the installed release 7 untouched",
                  fat_get(disk, ESP_START,
                          "/EFI/ThreadOS/initrd64.img") == initrd7)

            out = run(q, "run update rollback",
                      end="[update] done, exit code")
            check("a regular user cannot roll back the system",
                  "only an admin may restore the system" in out)
            out = run(q, "sudo run update rollback",
                      end="[update] done, exit code", password=True)
            check("an admin can restore the previous system",
                  "the previous system was restored" in out and
                  "[update] done, exit code 0" in out)
        finally:
            q.close()

        for path, want in old_files.items():
            check(f"{path} is restored byte for byte",
                  fat_get(disk, ESP_START, path) == want)

        q = boot_disk(disk, nvram, os.path.join(work, "s4.log"))
        try:
            q.wait_serial("The 64-bit system is ready", timeout=120)
            q.wait_serial("[net] DHCP", timeout=60)
            out = run(q, "run update check " + base + "rel/",
                      end="[update] done, exit code")
            check("the restored previous system boots with a pre-1.0 label",
                  "this system is version 0.1.0-" in out and
                  "(update sequence 0)" in out and
                  "a newer system is available" in out)

            q.type_line("desktop")
            q.wait_serial("The desktop is starting", timeout=30)
            check("the restored system reaches its desktop",
                  q.wait_desktop(timeout=40))
            q.launch_dock("gsettings")
            title = ""
            wx = wy = 0
            deadline = time.time() + 12
            while time.time() < deadline:
                for slot in range(8):
                    x, y = cascade(slot, GSET_W + 2,
                                   GSET_H + TITLE_H + 2)
                    title = q.window_title(x, y, GSET_W + 2)
                    if title == "Settings":
                        wx, wy = x, y
                        break
                if title == "Settings":
                    break
                time.sleep(0.25)
            check("Settings opens on the restored installed system",
                  title == "Settings")
            if title == "Settings":
                cx, cy = wx + 1, wy + CONTENT_DY
                q.click(cx + GSET_SIDE_W // 2,
                        cy + GSET_SIDE_Y0 + 8 * GSET_SIDE_H +
                        GSET_SIDE_H // 2)
                q.mouse_goto(600, 600)
                time.sleep(1.5)
                top = GSET_PANE_Y0 + GSET_INFO_H + 70
                baseline = cy + ui_font_baseline_in(UD_PX, top,
                                                    GSET_CHIP_H)
                shot = q.screen_raw()
                label = q.read_ui(cx + GSET_PANE_X, baseline,
                                 GSET_W - GSET_PANE_X - 8, px=UD_PX,
                                 fg=theme("dark")["accent_text"], screen=shot)
                check("Settings exposes the previous-system rollback action",
                      q.gset_heading(cx, cy, screen=shot) == "Updates" and
                      "Restore previous system" in label)
                installed_version = q.gset_info(cx, cy, 0, screen=shot)
                check("Settings shows the pre-1.0 display version",
                      installed_version.startswith("0.1.0-"))
        finally:
            q.close()
    finally:
        stop()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
