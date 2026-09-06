#!/usr/bin/env python3
"""ProtonOS smoke test: a disk in the USB socket (pendrive).

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
                   GUID_ESP4, GUID_PFS4)

STICK_MIB = 64
INSTALL_MIB = 400          # room for the EFI partition and ours
FROM_HOST = b"To pisal mtools na hoscie, nie my.\n"


def usb_disk_number(listing):
    """Which number the pendrive got in 'dyski'. It is not a constant:
    booting through UEFI adds a disk of the firmware's own, so the
    stick moves - and a test that assumed '2' would be testing the
    numbering rather than the disk."""
    m = re.search(r"dysk (\d+): USB \(pamiec masowa\)", listing)
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
                                    label="ZAHUBEM")
    fat_put(stick, start, b"za koncentratorem\n", "/zahubem.txt")

    try:
        if not make_iso64(d, iso, text=True):
            check("plyta dla proby z koncentratorem buduje sie", False)
            return
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, usbdisk=stick, usbhub=True, behind_hub=True,
                 qemu="qemu-system-x86_64")
        try:
            boot = q.wait_serial("proton64>", timeout=90)
            check("system znajduje koncentrator",
                  "Koncentrator USB" in boot)
            check("i dysk ZA nim", "Dysk USB:" in boot)

            out = q.command("dyski", "sektorow", timeout=25)
            usbno = usb_disk_number(out)
            check("dysk zza koncentratora jest na liscie jak kazdy inny "
                  "(dysk %d)" % usbno, usbno > 0)

            out = q.command("fat 1 %d" % usbno, "klaster", timeout=30)
            check("i da sie go czytac", "FAT32" in out and "ZAHUBEM" in out)
            out = q.command("fatcat /zahubem.txt", "koncentratorem",
                            timeout=20)
            check("razem z plikiem, ktory host na nim zostawil",
                  "za koncentratorem" in out)
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
                                    label="POZNIEJ")
    fat_put(stick, start, b"wlozony w trakcie pracy\n", "/pozniej.txt")

    try:
        if not make_iso64(d, iso, text=True):
            check("plyta dla proby z wkladaniem buduje sie", False)
            return
        # An xHCI controller with NOTHING in it - the stick comes later
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, usbkbd="xhci", qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=90)
            out = q.command("dyski", "sektorow", timeout=20)
            check("na poczatku pendrive'a nie ma na liscie",
                  "USB (pamiec masowa)" not in out)

            # Plugged in while the machine runs
            q.hmp("drive_add 0 if=none,id=pd,format=raw,file=" + stick)
            q.cmd({"execute": "device_add",
                   "arguments": {"driver": "usb-storage", "bus": "xhci.0",
                                 "drive": "pd", "id": "pendrive"}})
            found = q.wait_serial("Dysk USB", timeout=30)
            check("system zauwaza pendrive wlozony w trakcie pracy",
                  "Dysk USB" in found)

            out = q.command("dyski", "sektorow", timeout=20)
            usbno = usb_disk_number(out)
            check("i dopisuje go do listy dyskow (dysk %d)" % usbno,
                  usbno > 0)

            out = q.command("fat 1 %d" % usbno, "klaster", timeout=30)
            # A 32 MiB volume that a formatter was told to make FAT32
            # IS FAT32, however few clusters it has - and reading it by
            # cluster count alone showed an empty directory on a full
            # stick.
            check("da sie go od razu czytac, i to jako FAT32",
                  "FAT32" in out and "POZNIEJ" in out)
            out = q.command("fatcat /pozniej.txt", "wlozony", timeout=20)
            check("razem z plikiem, ktory host na nim zostawil",
                  "wlozony w trakcie pracy" in out)

            # And pulled out
            q.cmd({"execute": "device_del",
                   "arguments": {"id": "pendrive"}})
            gone = q.wait_serial("wyjety z gniazdka", timeout=30)
            check("system zauwaza wyjecie pendrive'a",
                  "wyjety z gniazdka" in gone)

            out = q.command("dyski", "sektorow", timeout=20)
            check("i mowi o nim, ze go nie ma, zamiast udawac",
                  "wyjety z gniazdka" in out)

            # The numbers must not move: somebody is reading that list
            check("numery pozostalych dyskow sie nie przesunely",
                  "dysk 1: AHCI" in out)

            out = q.command("fatls /", "proton64>", timeout=20)
            check("po wyjeciu nic nie jest juz podmontowane - zamiast "
                  "pustego katalogu jest odmowa",
                  "nic nie jest podmontowane" in out)

            # The machine is still alive afterwards
            out = q.command("uptime", "Uptime", timeout=20)
            check("system dziala dalej po wyjeciu dysku", "Uptime" in out)
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
            q.wait_serial("proton64>", timeout=90)
            check("koncentrator sam, bez niczego w nim",
                  "Koncentrator USB" in q.serial())
            out = q.command("dyski", "sektorow", timeout=25)
            check("i na razie zadnego dysku USB",
                  "USB (pamiec masowa)" not in out)

            q.hmp("drive_add 0 if=none,id=pd2,format=raw,file=" + stick)
            q.cmd({"execute": "device_add",
                   "arguments": {"driver": "usb-storage", "bus": "xhci.0",
                                 "port": "2.1", "drive": "pd2",
                                 "id": "wdoku"}})
            found = q.wait_serial("Dysk USB", timeout=40)
            check("pendrive wlozony do koncentratora tez zostaje "
                  "zauwazony", "Dysk USB" in found)

            out = q.command("dyski", "sektorow", timeout=25)
            usbno = usb_disk_number(out)
            check("i trafia na liste dyskow (dysk %d)" % usbno, usbno > 0)
            out = q.command("fat 1 %d" % usbno, "klaster", timeout=30)
            check("razem z tym, co na nim jest",
                  "FAT32" in out and "POZNIEJ" in out)
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
            print("POMINIETE: brak %s - bez instalacji na pendrive" % path)
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
        shutil.copy(KERNEL64, os.path.join(esp, "protonos64.bin"))
        shutil.copy(INITRD64, os.path.join(esp, "initrd64.img"))

        with open(own, "wb") as fh:
            fh.truncate(DISK_SIZE)
        # A stick with somebody's data on it - it is going away, and the
        # installer has to say so before it does.
        made, start = make_mbr_fat_disk(stick, mib=INSTALL_MIB, fat32=True,
                                        label="STAREDANE")
        fat_put(stick, start, b"zdjecia z wakacji" * 100, "/wazne.txt")

        shutil.copy(OVMF_VARS, nvram)
        q = Qemu(KERNEL, INITRD, esp=esp, disk=own, sata=True,
                 usbdisk=stick, serial=slog, qemu="qemu-system-x86_64",
                 uefi=(OVMF_CODE, nvram))
        try:
            q.wait_serial("System 64-bitowy gotowy", timeout=140)
            out = q.command("dyski", "sektorow", timeout=20)
            usbno = usb_disk_number(out)
            check("instalator ma z czego wybierac: wlasny dysk i pendrive"
                  " (pendrive to dysk %d)" % usbno, usbno > 0)
            if not usbno:
                return

            out = q.command("instaluj tekstem caly %d" % usbno,
                            "Wpisz KASUJ", timeout=60)
            check("plan dotyczy pendrive'a, nie dysku maszyny",
                  "USB" in out or ("dysk %d" % usbno) in out)
            out = q.command("KASUJ", "Gotowe", timeout=300)
            check("instalacja na pendrive dochodzi do konca",
                  "tablica partycji (GPT) - gotowe" in out
                  and "partycja EFI (FAT32) - gotowe" in out
                  and "/EFI/BOOT/BOOTX64.EFI - gotowe" in out)
        finally:
            q.close()

        # --- what came out, read by somebody else -------------------
        ok, parts, why = gpt_read(stick)
        check("pendrive ma teraz nasza tablice GPT%s"
              % ("" if ok else ": " + why), ok and len(parts) == 2)
        if len(parts) == 2:
            check("z partycja EFI i nasza",
                  parts[0][0] == GUID_ESP4 and parts[1][0] == GUID_PFS4)
        check("na partycji EFI pendrive'a jest FAT32",
              fat_kind(stick, 2048) == "FAT32")
        loader = fat_get(stick, 2048, "/EFI/BOOT/BOOTX64.EFI")
        with open(LOADER, "rb") as fh:
            want = fh.read()
        check("loader zapisany co do bajtu (%d B)"
              % (len(loader) if loader else 0), loader == want)
        check("stare dane sa nie do odczytania - dysk zostal skasowany",
              fat_get(stick, 2048, "/wazne.txt") is None)

        # --- and the proof: the firmware boots FROM the stick -------
        shutil.copy(OVMF_VARS, nvram)
        q2 = Qemu(KERNEL, INITRD, disk=stick, sata=True, serial=slog2,
                  qemu="qemu-system-x86_64", uefi=(OVMF_CODE, nvram))
        try:
            out = q2.wait_serial("protoboot: ProtonOS", timeout=90)
            check("firmware wybiera nasz loader z pendrive'a",
                  "protoboot" in out)
            out = q2.wait_serial("System 64-bitowy gotowy", timeout=140)
            check("i system z pendrive'a dochodzi do powloki",
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
    print("--- dysk w gniazdku USB ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()
    if not mtools_ok():
        print("POMINIETE: brak mtools - nie ma czym sprawdzic pendrive'a")
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")      # the machine's own disk (PFS)
    stick = tempfile.mktemp(suffix=".usb")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)

    try:
        check("plyta z jadrem 64-bitowym buduje sie",
              make_iso64(d, iso, text=True))
        if not os.path.exists(iso):
            return summary()

        # The stick: a partition table of the kind a stick really has,
        # a FAT32 on it, and one file put there by somebody else's tool.
        made, start = make_mbr_fat_disk(stick, mib=STICK_MIB, fat32=True,
                                        label="PENDRIVE")
        check("pendrive sformatowany przez mtools (nie przez nas)", made)
        if not made:
            return summary()
        check("plik od hosta zapisany na pendrive",
              fat_put(stick, start, FROM_HOST, "/od-hosta.txt"))

        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, disk=disk,
                 sata=True, usbdisk=stick, qemu="qemu-system-x86_64")
        try:
            boot = q.wait_serial("proton64>", timeout=90)

            # --- the device itself -----------------------------------
            check("system znajduje dysk w gniazdku USB",
                  "Dysk USB:" in boot)
            m = re.search(r"Dysk USB: (.*), (\d+) sektorow", boot)
            check("i podaje jego nazwe oraz pojemnosc"
                  + (" ('%s', %s sektorow)" % (m.group(1), m.group(2))
                     if m else ""), m is not None)
            # QEMU's disk is exactly as large as the file we made
            check("pojemnosc zgadza sie z rozmiarem obrazu (%d sektorow)"
                  % (STICK_MIB * 2048),
                  m is not None and int(m.group(2)) == STICK_MIB * 2048)

            # --- and the block layer sees it as a disk ---------------
            out = q.command("dyski", "sektorow", timeout=20)
            check("pendrive jest na liscie dyskow obok wlasnego dysku",
                  "USB (pamiec masowa)" in out and "AHCI" in out)
            usbno = usb_disk_number(out)
            check("ale to NIE on jest uzywany - system zostaje na swoim",
                  re.search(r"USB \(pamiec masowa\).*<- uzywany", out)
                  is None)
            check("system widzi na nim tablice partycji",
                  "FAT" in out or "partycja" in out.lower())

            # --- reading: a file written by mtools -------------------
            out = q.command("fat 1 %d" % usbno, "klaster", timeout=30)
            check("FAT na pendrivie podmontowany ze WSKAZANEGO dysku",
                  "dysk %d, partycja 1" % usbno in out)
            check("i rozpoznany jako FAT32 z etykieta PENDRIVE",
                  "FAT32" in out and "PENDRIVE" in out)

            out = q.command("fatls /", "pozycji", timeout=20)
            check("widzimy plik, ktory host tam zapisal",
                  "od-hosta.txt" in out)

            out = q.command("fatcat /od-hosta.txt", "mtools", timeout=20)
            check("tresc pliku z pendrive'a odczytana co do znaku",
                  FROM_HOST.decode().strip() in out)

            # --- and PFS on the machine's own disk still works -------
            # The whole point of mounting a FAT "from disk 2": the rest
            # of the system must not have moved.
            q.command("write proba.txt pendrive-nie-przestawil-dysku",
                      "zapisano", timeout=20)
            out = q.command("cat proba.txt", "pendrive", timeout=20)
            check("PFS na wlasnym dysku dziala dalej, mimo montowania USB",
                  "pendrive-nie-przestawil-dysku" in out)

            # --- writing: our file, read back by mtools --------------
            q.command("fatmkdir /ZSYSTEMU", "fatmkdir", timeout=20)
            q.command("fatput /ZSYSTEMU/witaj.txt czesc-z-protonos",
                      "fatput", timeout=20)
            out = q.command("fatcopy hello64 /ZSYSTEMU/hello64.bin",
                            "fatcopy", timeout=60)
            check("wieksze pisanie po USB dochodzi do konca (program z "
                  "initrd)", "skopiowano" in out or "B" in out)

            # Everything has to reach the disk before the machine dies
            q.command("fat 1 %d" % usbno, "klaster", timeout=30)  # flush
        finally:
            q.close()

        # --- the host looks at what came out ------------------------
        listing = fat_dir(stick, start, "::/ZSYSTEMU")
        check("mtools widzi katalog i pliki zapisane przez system",
              listing is not None and "WITAJ" in (listing or "").upper())

        got = fat_get(stick, start, "/ZSYSTEMU/witaj.txt")
        check("i tresc jest dokladnie ta, ktora system zapisal"
              + (" ('%s')" % got.decode(errors="replace").strip()
                 if got else ""),
              got is not None and b"czesc-z-protonos" in got)

        binary = fat_get(stick, start, "/ZSYSTEMU/hello64.bin")
        check("skopiowany program ma sensowna dlugosc i naglowek ELF"
              + (" (%d B)" % len(binary) if binary else ""),
              binary is not None and len(binary) > 1024 and
              binary[:4] == b"\x7fELF")

        # The file the host put there has to be untouched: writing to a
        # stick must add, not rearrange.
        old = fat_get(stick, start, "/od-hosta.txt")
        check("plik hosta przezyl pisanie po tym samym systemie plikow",
              old == FROM_HOST)

        # --- and the same disk one tier further away --------------
        print("--- pendrive za koncentratorem ---")
        behind_hub()

        # --- and the half that has nothing to do with booting -----
        print("--- pendrive wlozony i wyjety przy pracujacym systemie ---")
        hotplug()

        # --- and the thing a stick is for --------------------------
        print("--- instalacja systemu na pendrive ---")
        install_to_stick()
    finally:
        for path in (iso, slog, disk, stick):
            if os.path.exists(path):
                os.unlink(path)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
