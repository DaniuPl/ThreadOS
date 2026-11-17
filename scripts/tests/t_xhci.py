#!/usr/bin/env python3
"""ThreadOS smoke test: a USB keyboard on an xHCI controller.

xHCI is the third and last USB controller - the only one newer boards
have. It differs from UHCI not in details but in the division of labour:
there the driver laid transfer descriptors out in a frame list and
assigned addresses to devices itself, here it puts commands into a ring
and collects the answers through a second ring - of events.

We check two things at once:

  1. that the driver brings the keyboard to a conversation - a port
     reset, a slot assignment, an address, reading the configuration, the
     boot protocol, and then HID reports travelling through the transfer
     ring;
  2. that HIGHER UP no difference can be seen. The same group of checks
     as with UHCI (t_usbkbd64.py), on the same keyboard and the same HID
     code - the only difference is the bus underneath. The usbhc.c layer
     is to hide that, and that is exactly what we measure here.

Then the same controller in the shapes a real machine gives it: a
keyboard behind a hub, ten devices on one controller, and a start
through UEFI firmware, which places the controller's registers above
4 GiB - and a machine with no USB controller at all, which has to say
why there is no USB.

Run on its own or through scripts/run_tests.py, which starts all the
groups in parallel.
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
from ptest import check, summary, make_iso64, no64, _kernel_define

# Every console starts a shell of its own, and each prints its prompt on
# the serial port too
CONSOLES = _kernel_define("kernel/include/vga.h", "CONSOLE_COUNT")
# What 'hardware' ends with: the PCI list, whatever it says under it, and
# the prompt after that. A prompt alone does not mark the end - see
# wait_shells().
HARDWARE_END = re.compile(r"PCI:\n\r?(?:  \d+\.\d+  [^\n]*\n\r?)+[^>]*"
                          r"thread64> ")

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
# The loader of the build under test, which lies next to its kernel -
# not build/ in the repository, which may be another build altogether
LOADER = os.path.join(os.path.dirname(os.path.abspath(KERNEL64)),
                      "BOOTX64.EFI")


def type_text(q, text, delay=0.12):
    for ch in text:
        q.sendkey(ch)
        time.sleep(delay)
    q.sendkey("ret")
    time.sleep(0.25)


def wait_fresh(q, since, text, timeout=10):
    """What arrived on the serial port after 'since', once 'text' is in
    it (or when the time runs out - the assertion belongs to the
    caller)."""
    end = time.time() + timeout
    while time.time() < end:
        fresh = q.serial()[since:]
        if text in fresh:
            return fresh
        time.sleep(0.3)
    return q.serial()[since:]


def wait_shells(q, timeout=90):
    """The start, up to the prompt of the LAST console's shell.

    The first prompt is not the end of it: the shells of the other
    consoles print theirs when their turn comes, and one arriving after a
    command was typed would pass for the end of that command's answer."""
    start = q.wait_serial("thread64>", timeout=timeout)
    end = time.time() + 20
    while start.count("thread64>") < CONSOLES and time.time() < end:
        time.sleep(0.3)
        start = q.serial()
    return start


def hardware(q, typed, timeout=30):
    """Runs 'hardware' ('typed' types the line) and hands back its whole
    answer, read until the PCI list is closed by the prompt (or the time
    runs out - the assertions belong to the caller)."""
    before = len(q.serial())
    typed("hardware")
    end = time.time() + timeout
    while time.time() < end:
        fresh = q.serial()[before:]
        if HARDWARE_END.search(fresh):
            return fresh
        time.sleep(0.3)
    return q.serial()[before:]


def main():
    print("--- a USB keyboard on xHCI ---")

    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
    else:
        d = tempfile.mkdtemp()
        iso = tempfile.mktemp(suffix=".iso")
        slog = tempfile.mktemp(suffix=".log")
        try:
            check("the disc with the 64-bit kernel builds", make_iso64(d, iso))
            if not os.path.exists(iso):
                return summary()

            # An empty UHCI controller (-usb) beside the xHCI one, as on
            # boards from the years when both were fitted. The keyboard is
            # only on xHCI, so a selection layer that took the older
            # controller would leave this machine without a keyboard.
            q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog, usbkbd="xhci",
                     qemu="qemu-system-x86_64", devices=["-usb"])
            try:
                # The whole start, not just up to the keyboard's line,
                # which may still be half written
                start = wait_shells(q)
                # The selection layer says by itself what it is talking
                # through
                check("the keyboard was found on the xHCI controller",
                      re.search(r"A USB keyboard \(HID, xHCI\) on port \d+",
                                start) is not None)
                # Typed at the USB keyboard's pace
                said = hardware(q, lambda line: type_text(q, line))
                check("the machine has two USB controllers: the UHCI one "
                      "is really there",
                      said.count("class c.3  USB") == 2
                      and "8086:7020  class c.3  USB" in said)
                check("and the system took the xHCI one, not the UHCI one",
                      "USB:      xHCI, keyboard yes" in said)

                before = len(q.serial())
                type_text(q, "uptime")
                after = wait_fresh(q, before, "Uptime:")
                check("a command typed through xHCI runs",
                      "Uptime:" in after)

                # Several keys in a row: the reports have to go one after
                # another through the same transfer ring, not just the
                # first one
                before = len(q.serial())
                type_text(q, "help")
                help_text = wait_fresh(q, before, "Commands:")
                check("the whole word arrives character by character",
                      "help" in help_text and "Commands:" in help_text)

                # A capital letter comes from a modifier in the HID report -
                # that is a different path through a report than an
                # ordinary key code. QEMU holds a key for about a hundred
                # milliseconds, so with too fast a rate neighbouring keys
                # overlap; hence the slower pace and a second attempt.
                big_one = False
                for _ in range(3):
                    before = len(q.serial())
                    for ch in "echo":
                        q.sendkey(ch)
                        time.sleep(0.15)
                    for k in ("spc", "shift-a", "ret"):
                        q.sendkey(k)
                        time.sleep(0.25)
                    if "echo A" in wait_fresh(q, before, "echo A", timeout=5):
                        big_one = True
                        break
                check("the Shift modifier from the HID report gives a capital letter",
                      big_one)

                # Alt+F2 is a different path inside the kernel than an
                # ordinary character (a console switch, not the input
                # queue)
                q.sendkey("alt-f2")
                check("Alt+F2 from the xHCI keyboard switches consoles",
                      "console 2" in q.wait_fbcon("console 2", timeout=10))
            finally:
                q.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)
            if os.path.exists(iso):
                os.unlink(iso)
            if os.path.exists(slog):
                os.unlink(slog)

    # --- A hub between the machine and the keyboard -----------------
    #
    # What a docking station looks like from the machine's side, and a
    # thing that was simply invisible until hubs were understood: a
    # device behind a hub is not on any port of the controller, so
    # walking the controller's ports finds nothing. The hub's own ports
    # have to be asked about, given power and reset one by one - and
    # then the controller has to be told the WAY to what was found,
    # four bits per tier of hubs.
    if not no64():
        print("--- a keyboard behind a USB hub ---")
        dh = tempfile.mkdtemp()
        isoh = tempfile.mktemp(suffix=".iso")
        slogh = tempfile.mktemp(suffix=".log")
        try:
            check("the disc for the hub attempt builds",
                  make_iso64(dh, isoh, text=True))
            qh = Qemu(KERNEL, INITRD, cdrom=isoh, serial=slogh,
                      usbkbd="hub", qemu="qemu-system-x86_64")
            try:
                start = qh.wait_serial("thread64>", timeout=90)
                check("the system finds the hub and counts its sockets",
                      re.search(r"A USB hub in socket \d+: "
                                r"\d+ sockets more", start) is not None)

                m = re.search(r"A USB keyboard \(HID, xHCI\) on port "
                              r"(\d+)", start)
                check("and the keyboard BEHIND it - in a socket the "
                      "controller does not have"
                      + (" (port %s)" % m.group(1) if m else ""),
                      m is not None and int(m.group(1)) > 4)

                # And it is a keyboard, not a line in a log: what is
                # typed on it has to reach the shell.
                before = len(qh.serial())
                type_text(qh, "uptime")
                typed = wait_fresh(qh, before, "Uptime:")
                check("and what is typed on it reaches the shell",
                      "Uptime:" in typed)
            finally:
                qh.close()
        finally:
            shutil.rmtree(dh, ignore_errors=True)
            for f in (isoh, slogh):
                if os.path.exists(f):
                    os.unlink(f)

    # --- A laptop's worth of devices on one controller --------------
    #
    # A laptop has a webcam, a card reader and the Bluetooth half of its
    # wireless card on this bus before anybody plugs anything in. The
    # controller used to keep four slots, so a fifth device got none;
    # and every attach and port reset sends an event the driver kept
    # in a queue of eight, until the answer to a command found no room
    # and every command after that "never finished". Ten devices here,
    # the keyboard in the last socket, and it has to type.
    if not no64():
        print("--- ten devices on one xHCI controller ---")
        dl = tempfile.mkdtemp()
        isol = tempfile.mktemp(suffix=".iso")
        slogl = tempfile.mktemp(suffix=".log")
        sticks = [tempfile.mktemp(suffix=".img") for _ in range(3)]
        for s in sticks:
            with open(s, "wb") as fh:
                fh.truncate(8 * 1024 * 1024)
        # QEMU puts the USB 3.0 port first, so its USB 2.0 port N is the
        # controller's port N + 1 - our socket N + 1
        devs = ["-device", "qemu-xhci,id=xhci,p2=15,p3=1",
                "-device", "usb-tablet,bus=xhci.0,port=1",
                "-device", "usb-wacom-tablet,bus=xhci.0,port=2",
                "-audiodev", "none,id=usbsnd",
                "-device", "usb-audio,audiodev=usbsnd,bus=xhci.0,port=3",
                "-device", "usb-tablet,bus=xhci.0,port=4",
                "-device", "usb-hub,bus=xhci.0,port=5"]
        for i, s in enumerate(sticks):
            devs += ["-drive", "if=none,id=st%d,format=raw,file=%s" % (i, s),
                     "-device", "usb-storage,drive=st%d,bus=xhci.0,port=%d"
                     % (i, 6 + i)]
        devs += ["-device", "usb-mouse,bus=xhci.0,port=9",
                 "-device", "usb-kbd,bus=xhci.0,port=10"]
        try:
            check("the disc for the ten-device machine builds",
                  make_iso64(dl, isol, text=True))
            ql = Qemu(KERNEL, INITRD, cdrom=isol, serial=slogl,
                      qemu="qemu-system-x86_64", devices=devs)
            try:
                start = ql.wait_serial("thread64>", timeout=150)
                check("the keyboard in the last socket is found",
                      re.search(r"A USB keyboard \(HID, xHCI\) on port 11\b",
                                start) is not None)
                before = len(ql.serial())
                type_text(ql, "uptime")
                typed = wait_fresh(ql, before, "Uptime:")
                check("and what is typed on it reaches the shell",
                      "Uptime:" in typed)

                said = ql.command("hardware", "PCI:", timeout=30)
                check("'hardware' lists what is in each socket: the "
                      "keyboard in socket 11",
                      "socket 11: 0627:0001, input (HID)" in said)
                check("the three sticks as storage, the hub as a hub",
                      said.count(", storage, high speed") == 3
                      and "socket 6: 0409:55aa, hub" in said)
                check("and the sound card by its interface, its device "
                      "class being 0", "socket 4: 46f4:0002, audio" in said)
                check("ten devices listed, one per socket",
                      len(re.findall(r"socket \d+: [0-9a-f]{4}:[0-9a-f]{4}",
                                     said)) == 10)
                if not re.search(r"socket 11: 0627:0001", said):
                    print(said)
            finally:
                ql.close()
        finally:
            shutil.rmtree(dl, ignore_errors=True)
            for f in [isol, slogl] + sticks:
                if os.path.exists(f):
                    os.unlink(f)

    # --- A machine with no USB controller at all --------------------
    #
    # "USB: none" is not an answer anybody can act on: "there is no xHCI
    # controller on the PCI bus" and "the controller will not reset" call
    # for different things to be done. A machine without USB is the
    # only one on which that reason is printed, so it gets a boot of its
    # own.
    if not no64():
        print("--- a machine with no USB controller ---")
        dn = tempfile.mkdtemp()
        ison = tempfile.mktemp(suffix=".iso")
        slogn = tempfile.mktemp(suffix=".log")
        try:
            check("the disc for the machine without USB builds",
                  make_iso64(dn, ison, text=True))
            qn = Qemu(KERNEL, INITRD, cdrom=ison, serial=slogn,
                      qemu="qemu-system-x86_64")
            try:
                wait_shells(qn)
                said = hardware(qn, qn.type_line)
                check("the PCI list has no USB controller on it",
                      "PCI:" in said and "class c.3  USB" not in said)
                check("'hardware' says there is no USB",
                      "USB:      none, keyboard no, mouse no" in said)
                check("and says why: no xHCI controller on the PCI bus",
                      re.search(r"USB:      none[^\n]*\n\s*\(there is no "
                                r"xHCI controller on the PCI bus\)",
                                said) is not None)
            finally:
                qn.close()
        finally:
            shutil.rmtree(dn, ignore_errors=True)
            for f in (ison, slogn):
                if os.path.exists(f):
                    os.unlink(f)

    # --- And the same machine started through UEFI ------------------
    #
    # This is where a real laptop starts, and it turned out to be a
    # different machine from the one booted off a disc: UEFI firmware
    # gives the controller a memory window ABOVE 4 GiB, and a driver
    # that refuses such an address has no USB at all - so no keyboard
    # and no mouse on a machine that has nothing else. Nothing caught
    # it, because every USB test until now booted through the old BIOS.
    reason = no64()
    if not reason and os.path.exists(OVMF_CODE) and os.path.exists(OVMF_VARS):
        print("--- a USB keyboard on xHCI, booted through UEFI ---")
        esp = tempfile.mkdtemp()
        lbuild = tempfile.mkdtemp()
        nvram = tempfile.mktemp(suffix=".fd")
        slogu = tempfile.mktemp(suffix=".log")
        try:
            loader = LOADER
            tools = shutil.which("clang") and shutil.which("lld-link")
            if not os.path.exists(loader) and tools:
                # The build under test was made without its loader. One is
                # built here into a directory of its own - never into
                # build/, whose images other groups may be booting right
                # now.
                loader = os.path.join(lbuild, "BOOTX64.EFI")
                made = subprocess.run(["make", "BUILD=" + lbuild, "efi"],
                                      cwd=ROOT, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, text=True,
                                      errors="replace")
                # With the tools there, no loader means a loader that does
                # not compile - a failure, not a machine to skip
                check("the UEFI loader builds", os.path.exists(loader))
                if not os.path.exists(loader):
                    print("      " + "\n      ".join(
                        made.stdout.strip().splitlines()[-15:]))
            if not os.path.exists(loader):
                if not tools:
                    print("SKIPPED: no BOOTX64.EFI next to the kernel, and "
                          "nothing to build one with (apt install clang "
                          "lld)")
            else:
                os.makedirs(os.path.join(esp, "EFI", "BOOT"))
                shutil.copy(loader,
                            os.path.join(esp, "EFI", "BOOT", "BOOTX64.EFI"))
                shutil.copy(KERNEL64, os.path.join(esp, "threados64.bin"))
                shutil.copy(INITRD64, os.path.join(esp, "initrd64.img"))
                stick_starts_live(esp)   # this group types at the shell
                shutil.copy(OVMF_VARS, nvram)

                q4 = Qemu(KERNEL, INITRD, esp=esp, serial=slogu,
                          usbkbd="xhci", qemu="qemu-system-x86_64",
                          uefi=(OVMF_CODE, nvram))
                try:
                    # Not wait_serial, which raises: a start that stops
                    # half way is to fail the checks below and say where
                    # it stopped, not end the group with a traceback
                    start = wait_fresh(q4, 0, "The 64-bit system is ready",
                                       timeout=140)
                    ready = "The 64-bit system is ready" in start
                    check("the system starts through UEFI firmware", ready)
                    if not ready:
                        # The kernel's own self-tests name what went wrong
                        for ln in start.splitlines():
                            if "[ !! ]" in ln:
                                print("      " + ln.strip())
                    # Firmware hands the kernel a different machine than
                    # GRUB does, and this is the start on which the
                    # threads have been seen not to run at all - with
                    # "OK" in front of two counters at zero
                    m = re.search(r"\[ (OK|!!) \] Threads counting side by "
                                  r"side: A=(\d+), B=(\d+)", start)
                    check("the thread self-test says OK only when both "
                          "threads counted"
                          + (" (%s, A=%s, B=%s)" % m.groups() if m else ""),
                          m is not None and (m.group(1) == "OK") ==
                          (int(m.group(2)) > 0 and int(m.group(3)) > 0))
                    check("after a UEFI boot the USB keyboard is found",
                          re.search(r"A USB keyboard \(HID, xHCI\)",
                                    start) is not None)
                    # threadboot starts the desktop, and its sign-in
                    # screen takes every key; signed in, with no window
                    # focused, the keys reach the console's shell
                    q4.wait_desktop()
                    said = q4.command("hardware", "PCI:", timeout=30)
                    check("and 'hardware' no longer says 'USB: none'",
                          "USB:      xHCI" in said)
                finally:
                    q4.close()
        finally:
            shutil.rmtree(esp, ignore_errors=True)
            shutil.rmtree(lbuild, ignore_errors=True)
            for f in (nvram, slogu):
                if os.path.exists(f):
                    os.unlink(f)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
