#!/usr/bin/env python3
"""ThreadOS smoke test: a disk in the USB socket (pendrive).

The bus so far carried a keyboard and a mouse - devices that say a few
bytes when asked. A disk is the other kind of traffic: bulk endpoints,
data in both directions, and a SCSI command inside every transfer.

What makes this worth its own group is that "the system saw a disk" is
not the interesting part. The interesting part is that it is a DISK
like any other: the same block layer, the same partition table, the
same FAT - so a file written to the stick by the host has to be
readable from inside the system, and a file written from inside the
system has to be found on the host afterwards, with mtools, which
shares no line of code with us.

The image is an ordinary file. Whatever the guest writes to the stick
lands in it, so the result can be read back here rather than taken on
trust.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, no64, make_iso64, make_mbr_fat_disk,
                   mtools_ok, fat_put, fat_get, fat_dir, fat_kind, gpt_read,
                   GUID_ESP4, GUID_TFS4)

STICK_MIB = 64
INSTALL_MIB = 400          # room for the EFI partition and ours
FROM_HOST = b"mtools wrote this on the host, not us.\n"


def usb_disk_number(listing):
    """Which number the pendrive got in 'disks'. It is not a constant:
    booting through UEFI adds a disk of the firmware's own, so the
    stick moves - and a test that assumed '2' would be testing the
    numbering rather than the disk."""
    m = re.search(r"disk (\d+): USB \(mass storage\)", listing)
    return int(m.group(1)) if m else 0


OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
LOADER = os.path.join(ROOT, "build", "BOOTX64.EFI")


def behind_hub():
    """The same stick, but in a hub plugged into the machine.

    A disk behind a hub is on no port of the controller at all, so
    walking the controller's ports finds nothing - which is what a
    docking station looked like to this system until the hubs
    themselves were understood. Everything above the bus has to see no
    difference: the same list of disks, the same FAT, the same file."""
    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    stick = tempfile.mktemp(suffix=".usb")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    made, start = make_mbr_fat_disk(stick, mib=32, fat32=True,
                                    label="BEHINDHUB")
    fat_put(stick, start, b"behind the hub\n", "/behindhub.txt")

    try:
        if not make_iso64(d, iso, text=True):
            check("the disc for the hub attempt builds", False)
            return
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, usbdisk=stick, usbhub=True, behind_hub=True,
                 qemu="qemu-system-x86_64")
        try:
            boot = q.wait_serial("thread64>", timeout=90)
            check("the system finds the hub",
                  "A USB hub" in boot)
            check("and the disk BEHIND it", "USB disk:" in boot)

            out = q.command("disks", "sectors", timeout=25)
            usbno = usb_disk_number(out)
            check("the disk behind the hub is on the list like any other "
                  "(disk %d)" % usbno, usbno > 0)

            out = q.command("fat 1 %d" % usbno, "cluster", timeout=30)
            check("and it can be read", "FAT32" in out and "BEHINDHUB" in out)
            out = q.command("fatcat /behindhub.txt", "the hub",
                            timeout=20)
            check("together with the file the host left on it",
                  "behind the hub" in out)
        finally:
            q.close()
    finally:
        for path in (iso, slog, disk, stick):
            if os.path.exists(path):
                os.unlink(path)


def hotplug():
    """A stick pushed in three hours after the machine started.

    This is the half of "a disk in the socket" that has nothing to do
    with booting, and the one a person actually does: the machine is
    running, the socket is empty, and then it is not. QEMU can be told
    to plug a device in while it runs, which is why this can be tested
    at all rather than only asserted about.

    And the other direction, which matters more: after the stick is
    pulled out, the system must stop offering it. A disk that answers
    from memory after it has left the building is how a filesystem gets
    written to nothing."""
    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    stick = tempfile.mktemp(suffix=".usb")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)

    made, start = make_mbr_fat_disk(stick, mib=32, fat32=True,
                                    label="LATER")
    fat_put(stick, start, b"plugged in while it runs\n", "/later.txt")

    try:
        if not make_iso64(d, iso, text=True):
            check("the disc for the plugging attempt builds", False)
            return
        # An xHCI controller with NOTHING in it - the stick comes later
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, usbkbd="xhci", qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=90)
            out = q.command("disks", "sectors", timeout=20)
            check("at the start the stick is not on the list",
                  "USB (mass storage)" not in out)

            # Plugged in while the machine runs
            q.hmp("drive_add 0 if=none,id=pd,format=raw,file=" + stick)
            q.cmd({"execute": "device_add",
                   "arguments": {"driver": "usb-storage", "bus": "xhci.0",
                                 "drive": "pd", "id": "pendrive"}})
            found = q.wait_serial("USB disk", timeout=30)
            check("the system notices a stick plugged in while it runs",
                  "USB disk" in found)

            out = q.command("disks", "sectors", timeout=20)
            usbno = usb_disk_number(out)
            check("and adds it to the disk list (disk %d)" % usbno,
                  usbno > 0)

            out = q.command("fat 1 %d" % usbno, "cluster", timeout=30)
            # A 32 MiB volume that a formatter was told to make FAT32
            # IS FAT32, however few clusters it has - and reading it by
            # cluster count alone showed an empty directory on a full
            # stick.
            check("it can be read at once, and as a FAT32",
                  "FAT32" in out and "LATER" in out)
            out = q.command("fatcat /later.txt", "plugged in", timeout=20)
            check("together with the file the host left on it",
                  "plugged in while it runs" in out)

            # And pulled out
            q.cmd({"execute": "device_del",
                   "arguments": {"id": "pendrive"}})
            gone = q.wait_serial("pulled out of socket", timeout=30)
            check("the system notices the stick being pulled out",
                  "pulled out of socket" in gone)

            out = q.command("disks", "sectors", timeout=20)
            check("and it says it is gone instead of pretending",
                  "pulled out of the socket" in out)

            # The numbers must not move: somebody is reading that list
            check("the numbers of the other disks did not shift",
                  "disk 1: AHCI" in out)

            out = q.command("fatls /", "thread64>", timeout=20)
            check("after the pull nothing is mounted any more - instead of "
                  "an empty directory there is a refusal",
                  "nothing is mounted" in out)

            # The machine is still alive afterwards
            out = q.command("uptime", "Uptime", timeout=20)
            check("the system still runs after the disk is pulled out", "Uptime" in out)
        finally:
            q.close()

        # --- the same, but into a hub -----------------------------
        #
        # A stick pushed into a docking station rather than into the
        # machine. The hub says by itself that something changed on one
        # of its ports - it has an endpoint for exactly that - and the
        # difference from the socket in the machine has to end there.
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, usbhub=True, qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=90)
            check("the hub alone, with nothing in it",
                  "A USB hub" in q.serial())
            out = q.command("disks", "sectors", timeout=25)
            check("and no USB disk for now",
                  "USB (mass storage)" not in out)

            q.hmp("drive_add 0 if=none,id=pd2,format=raw,file=" + stick)
            q.cmd({"execute": "device_add",
                   "arguments": {"driver": "usb-storage", "bus": "xhci.0",
                                 "port": "2.1", "drive": "pd2",
                                 "id": "wdoku"}})
            found = q.wait_serial("USB disk", timeout=40)
            check("a stick put into the hub is "
                  "noticed", "USB disk" in found)

            out = q.command("disks", "sectors", timeout=25)
            usbno = usb_disk_number(out)
            check("and it lands on the disk list (disk %d)" % usbno, usbno > 0)
            out = q.command("fat 1 %d" % usbno, "cluster", timeout=30)
            check("together with what is on it",
                  "FAT32" in out and "LATER" in out)
        finally:
            q.close()
    finally:
        for path in (iso, slog, disk, stick):
            if os.path.exists(path):
                os.unlink(path)


def install_to_stick():
    """The point of a stick: making a bootable one from inside a running
    system, and then booting it.

    The machine is started from UEFI (that is where the installer takes
    the files from) with the stick in the socket as the SECOND disk -
    and the installer is told to take that one. Everything afterwards
    is read by somebody else: the partition table by our own GPT
    reader, the files by mtools, and the whole thing by the firmware,
    which either boots it or does not."""
    if not os.path.exists(LOADER):
        subprocess.run(["make", "efi"], cwd=ROOT, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    for path in (LOADER, OVMF_CODE, OVMF_VARS):
        if not os.path.exists(path):
            print("SKIPPED: no %s - no installing onto a stick" % path)
            return

    esp = tempfile.mkdtemp()
    own = tempfile.mktemp(suffix=".img")       # the machine's own disk
    stick = tempfile.mktemp(suffix=".usb")
    slog = tempfile.mktemp(suffix=".log")
    slog2 = tempfile.mktemp(suffix=".log")
    nvram = tempfile.mktemp(suffix=".fd")

    try:
        os.makedirs(os.path.join(esp, "EFI", "BOOT"))
        shutil.copy(LOADER, os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI"))
        shutil.copy(KERNEL64, os.path.join(esp, "threados64.bin"))
        shutil.copy(INITRD64, os.path.join(esp, "initrd64.img"))

        with open(own, "wb") as fh:
            fh.truncate(DISK_SIZE)
        # A stick with somebody's data on it - it is going away, and the
        # installer has to say so before it does.
        made, start = make_mbr_fat_disk(stick, mib=INSTALL_MIB, fat32=True,
                                        label="OLDDATA")
        fat_put(stick, start, b"holiday photographs" * 100, "/important.txt")

        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, esp=esp, disk=own, sata=True,
                 usbdisk=stick, serial=slog, qemu="qemu-system-x86_64",
                 uefi=(OVMF_CODE, nvram))
        try:
            q.wait_serial("The 64-bit system is ready", timeout=140)
            out = q.command("disks", "sectors", timeout=20)
            usbno = usb_disk_number(out)
            check("the installer has a choice: its own disk and the stick"
                  " (the stick is disk %d)" % usbno, usbno > 0)
            if not usbno:
                return

            out = q.command("install text whole %d" % usbno,
                            "Type ERASE", timeout=60)
            check("the plan is about the stick, not the machine's disk",
                  "USB" in out or ("disk %d" % usbno) in out)
            out = q.command("ERASE", "Done", timeout=300)
            check("installing onto the stick runs to the end",
                  "partition table (GPT) - done" in out
                  and "EFI partition (FAT32) - done" in out
                  and "/EFI/BOOT/BOOTX64.EFI - done" in out)
        finally:
            q.close()

        # --- what came out, read by somebody else -------------------
        ok, parts, why = gpt_read(stick)
        check("the stick has our GPT now%s"
              % ("" if ok else ": " + why), ok and len(parts) == 2)
        if len(parts) == 2:
            check("with an EFI partition and ours",
                  parts[0][0] == GUID_ESP4 and parts[1][0] == GUID_TFS4)
        check("the stick's EFI partition holds a FAT32",
              fat_kind(stick, 2048) == "FAT32")
        loader = fat_get(stick, 2048, "/EFI/BOOT/BOOTX64.EFI")
        with open(LOADER, "rb") as fh:
            want = fh.read()
        check("the loader was written byte for byte (%d B)"
              % (len(loader) if loader else 0), loader == want)
        check("the old data cannot be read - the disk was erased",
              fat_get(stick, 2048, "/important.txt") is None)

        # --- and the proof: the firmware boots FROM the stick -------
        shutil.copy(OVMF_VARS, nvram)
        q2 = Qemu(KERNEL, INITRD, disk=stick, sata=True, serial=slog2,
                  qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            out = q2.wait_serial("threadboot: ThreadOS", timeout=90)
            check("the firmware picks our loader off the stick",
                  "threadboot" in out)
            out = q2.wait_serial("The 64-bit system is ready", timeout=140)
            check("and the system on the stick reaches the shell",
                  "Multiboot OK (Multiboot 2)" in out)
        finally:
            q2.close()
    finally:
        shutil.rmtree(esp, ignore_errors=True)
        for f in (own, stick, slog, slog2, nvram):
            if os.path.exists(f):
                os.unlink(f)


def main():
    print()
    print("--- a disk in a USB socket ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    if not mtools_ok():
        print("SKIPPED: no mtools - nothing to check the stick with")
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")      # the machine's own disk (TFS)
    stick = tempfile.mktemp(suffix=".usb")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)

    try:
        check("the disc with the 64-bit kernel builds",
              make_iso64(d, iso, text=True))
        if not os.path.exists(iso):
            return summary()

        # The stick: a partition table of the kind a stick really has,
        # a FAT32 on it, and one file put there by somebody else's tool.
        made, start = make_mbr_fat_disk(stick, mib=STICK_MIB, fat32=True,
                                        label="PENDRIVE")
        check("the stick was formatted by mtools (not by us)", made)
        if not made:
            return summary()
        check("the host's file was written onto the stick",
              fat_put(stick, start, FROM_HOST, "/od-hosta.txt"))

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, usbdisk=stick, qemu="qemu-system-x86_64")
        try:
            boot = q.wait_serial("thread64>", timeout=90)

            # --- the device itself -----------------------------------
            check("the system finds the disk in the USB socket",
                  "USB disk:" in boot)
            m = re.search(r"USB disk: (.*), (\d+) sectors", boot)
            check("and gives its name and its capacity"
                  + (" ('%s', %s sectors)" % (m.group(1), m.group(2))
                     if m else ""), m is not None)
            # QEMU's disk is exactly as large as the file we made
            check("the capacity matches the image size (%d sectors)"
                  % (STICK_MIB * 2048),
                  m is not None and int(m.group(2)) == STICK_MIB * 2048)

            # --- and the block layer sees it as a disk ---------------
            out = q.command("disks", "sectors", timeout=20)
            check("the stick is on the disk list beside our own disk",
                  "USB (mass storage)" in out and "AHCI" in out)
            usbno = usb_disk_number(out)
            check("but it is NOT the one in use - the system stays on its own",
                  re.search(r"USB \(mass storage\).*<- in use", out)
                  is None)
            check("the system sees a partition table on it",
                  "FAT" in out or "partition" in out.lower())

            # --- reading: a file written by mtools -------------------
            out = q.command("fat 1 %d" % usbno, "cluster", timeout=30)
            check("the FAT on the stick was mounted from the NAMED disk",
                  "disk %d, partition 1" % usbno in out)
            check("and recognised as a FAT32 labelled PENDRIVE",
                  "FAT32" in out and "PENDRIVE" in out)

            out = q.command("fatls /", "entries", timeout=20)
            check("we see the file the host wrote there",
                  "od-hosta.txt" in out)

            out = q.command("fatcat /od-hosta.txt", "mtools", timeout=20)
            check("the content of the file from the stick was read character for character",
                  FROM_HOST.decode().strip() in out)

            # --- and TFS on the machine's own disk still works -------
            # The whole point of mounting a FAT "from disk 2": the rest
            # of the system must not have moved.
            q.command("write attempt.txt the-stick-did-not-move-the-disk",
                      "wrote", timeout=20)
            out = q.command("cat attempt.txt", "pendrive", timeout=20)
            check("the TFS on our own disk still works, despite the USB mount",
                  "the-stick-did-not-move-the-disk" in out)

            # --- writing: our file, read back by mtools --------------
            q.command("fatmkdir /FROMSYSTEM", "fatmkdir", timeout=20)
            q.command("fatput /FROMSYSTEM/hello.txt hello-from-threados",
                      "fatput", timeout=20)
            out = q.command("fatcopy hello64 /FROMSYSTEM/hello64.bin",
                            "fatcopy", timeout=60)
            check("a bigger write over USB runs to the end (a program from "
                  "initrd)", "skopiowano" in out or "B" in out)

            # Everything has to reach the disk before the machine dies
            q.command("fat 1 %d" % usbno, "cluster", timeout=30)  # flush
        finally:
            q.close()

        # --- the host looks at what came out ------------------------
        listing = fat_dir(stick, start, "::/FROMSYSTEM")
        check("mtools sees the directory and the files the system wrote",
              listing is not None and "HELLO" in (listing or "").upper())

        got = fat_get(stick, start, "/FROMSYSTEM/hello.txt")
        check("and the content is exactly what the system wrote"
              + (" ('%s')" % got.decode(errors="replace").strip()
                 if got else ""),
              got is not None and b"hello-from-threados" in got)

        binary = fat_get(stick, start, "/FROMSYSTEM/hello64.bin")
        check("the copied program has a sensible length and an ELF header"
              + (" (%d B)" % len(binary) if binary else ""),
              binary is not None and len(binary) > 1024 and
              binary[:4] == b"\x7fELF")

        # The file the host put there has to be untouched: writing to a
        # stick must add, not rearrange.
        old = fat_get(stick, start, "/od-hosta.txt")
        check("the host's file survived our writing on the same filesystem",
              old == FROM_HOST)

        # --- and the same disk one tier further away --------------
        print("--- a stick behind a hub ---")
        behind_hub()

        # --- and the half that has nothing to do with booting -----
        print("--- a stick plugged in and pulled out while the system runs ---")
        hotplug()

        # --- and the thing a stick is for --------------------------
        print("--- installing the system onto a stick ---")
        install_to_stick()
    finally:
        for path in (iso, slog, disk, stick):
            if os.path.exists(path):
                os.unlink(path)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
