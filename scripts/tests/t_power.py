#!/usr/bin/env python3
"""ThreadOS smoke test: the firmware's AML, run by ACPICA.

A laptop's battery and its power button are not in the ACPI tables the
kernel reads by hand: they are AML methods the firmware writes, and
kernel/power.c runs them on ACPICA. QEMU's machine has a power button
but no battery, so the test gives it one: a battery device written in
ASL here, compiled by Intel's own compiler (iasl, the one firmware
vendors use) and handed to QEMU as an extra SSDT. What the system says
about that battery is then checked against numbers the test chose.

  a battery with the older _BIF and a _BST that says charging: 73%,
  29200 of 40000 mWh

  one with _BIX whose last full charge is unknown (0xFFFFFFFF): the
  design capacity counts instead - 15000 of 60000 mWh, 25%, discharging

  the power button (QEMU's system_powerdown, which is what pressing it
  does): with no desktop the machine switches off; on the desktop it
  asks "Shut down the computer?" first, and the machine stays on until
  the answer

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
from ptest import check, summary, no64

DARK = theme("dark")

BIF_BATTERY = """
DefinitionBlock ("", "SSDT", 2, "THREAD", "BATTERY", 1)
{
    Scope (\\_SB)
    {
        Device (BAT0)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, One)
            Method (_STA, 0, NotSerialized) { Return (0x1F) }
            Method (_BIF, 0, NotSerialized)
            {
                Return (Package (0x0D) {
                    Zero,            // mWh
                    50000,           // design capacity
                    40000,           // last full charge
                    One,             // rechargeable
                    12000, 4000, 2000, 100, 100,
                    "TB01", "0001", "LION", "ThreadOS test" })
            }
            Method (_BST, 0, NotSerialized)
            {
                Return (Package (0x04) { 2, 1000, 29200, 12000 })
            }
        }
    }
}
"""

BIX_BATTERY = """
DefinitionBlock ("", "SSDT", 2, "THREAD", "BATTERY", 1)
{
    Scope (\\_SB)
    {
        Device (BAT0)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, One)
            Method (_STA, 0, NotSerialized) { Return (0x1F) }
            // Named and returned, not returned as it stands: the
            // iasl Ubuntu ships (20230628) rejects the last element of
            // any literal _BIX package; ACPICA checks it when it runs
            Name (BIXP, Package (0x15) {
                One,             // revision (ACPI 6: swapping, last)
                Zero,            // mWh
                60000,           // design capacity
                0xFFFFFFFF,      // last full charge: unknown
                One,
                12000, 6000, 3000, 25, 95000,
                1000, 1000, 1000, 1000, 100, 100,
                "TB02", "0002", "LION", "ThreadOS test",
                Zero })
            Method (_BIX, 0, NotSerialized) { Return (BIXP) }
            Method (_BST, 0, NotSerialized)
            {
                Return (Package (0x04) { One, 1500, 15000, 11800 })
            }
        }
    }
}
"""


def compile_ssdt(work, name, asl):
    src = os.path.join(work, name + ".asl")
    with open(src, "w") as fh:
        fh.write(asl)
    out = subprocess.run(["iasl", "-p", os.path.join(work, name), src],
                         capture_output=True, text=True)
    aml = os.path.join(work, name + ".aml")
    return aml if out.returncode == 0 and os.path.exists(aml) else None


def acpica_up(q):
    """The line ACPICA's start ends with, or '' if it never came"""
    try:
        q.wait_serial("[ OK ] ACPI: the firmware's AML runs", timeout=120)
    except Exception:
        return ""
    for line in q.serial().splitlines():
        if "the firmware's AML runs" in line:
            return line
    return ""


def battery_line(q):
    out = q.command("battery", "battery:", timeout=20)
    for line in out.splitlines():
        if line.startswith("battery:"):
            return line.strip()
    return ""


def question(q):
    """The question on the desktop's card and its button, as
    t_session.py reads them"""
    W, H = q.size()
    ax, ay = (W - ASK_W) // 2, (H - ASK_H) // 2
    btn_y = ay + ASK_H - ASK_PAD - ASK_BTN_H
    do_x = ax + ASK_W - ASK_PAD - ASK_BTN_W
    scr = q.screen_raw()
    title = q.read_ui(ax + ASK_PAD + 40 + 14, ay + ASK_TITLE_Y, 260, px=15,
                      face="bold", fg=DARK["app_text"], screen=scr)
    act = q.find_ui(do_x + 2, do_x + ASK_BTN_W - 2,
                    ui_font_baseline_in(UI_PX, btn_y, ASK_BTN_H), px=UI_PX,
                    face="bold", fg=DARK["accent_text"], screen=scr)
    return title, act


def powerdown(q):
    q.cmd({"execute": "system_powerdown"})


def text_mode(work, aml):
    print("--- _BIF, charging; the power button with no desktop ---")
    q = Qemu(KERNEL, INITRD, append=TEXT_MODE,
             serial=os.path.join(work, "bif.log"),
             devices=["-acpitable", "file=" + aml])
    try:
        q.wait_serial("thread64>", timeout=90)
        up = acpica_up(q)
        check("ACPICA runs the firmware's AML and finds the battery (%s)"
              % up.strip(), "a battery" in up and "objects" in up)
        got = battery_line(q)
        check("the battery as its _BIF and _BST describe it (%s)" % got,
              got == "battery: 73%, charging (29200 of 40000 mWh)")

        seen = len(q.serial())
        powerdown(q)
        ended = q.wait_exit(timeout=30)
        said = q.serial()[seen:]
        check("the power button switches a machine with no desktop off "
              "(QEMU ended: %s)" % ended,
              ended and "[acpi] the power button" in said and
              "no desktop to ask" in said)
    finally:
        q.close()


def desktop(work, aml):
    print("--- _BIX, discharging, last full charge unknown; the power "
          "button on the desktop ---")
    q = Qemu(KERNEL, INITRD, serial=os.path.join(work, "bix.log"),
             devices=["-acpitable", "file=" + aml])
    try:
        check("the desktop came up", q.wait_desktop())
        up = acpica_up(q)
        check("ACPICA starts beside the desktop", "a battery" in up)
        q.mouse_home()

        seen = len(q.serial())
        powerdown(q)
        got = ("", "")
        end = time.time() + 20
        while time.time() < end and got[0] != "Shut down the computer?":
            time.sleep(0.5)
            got = question(q)
        check("the power button asks first (%s)" % (got,),
              got == ("Shut down the computer?", "Shut Down"))
        time.sleep(3)
        check("and the machine stays on while it asks",
              q.proc.poll() is None and
              "[acpi] the power button" in q.serial()[seen:])

        q.sendkey("esc")
        time.sleep(1)
        q.click_desktop()
        got = battery_line(q)
        check("_BIX read, and the design capacity stands in for the "
              "unknown last full charge (%s)" % got,
              got == "battery: 25%, discharging (15000 of 60000 mWh)")

        seen = len(q.serial())
        powerdown(q)
        end = time.time() + 20
        while time.time() < end and question(q)[0] != \
                "Shut down the computer?":
            time.sleep(0.5)
        q.sendkey("ret")
        ended = q.wait_exit(timeout=25)
        check("pressed again and answered, it switches off (QEMU ended: %s)"
              % ended, ended and "Powering off" in q.serial()[seen:])
    finally:
        q.close()


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    if not shutil.which("iasl"):
        print("SKIPPED: iasl is not installed (apt install acpica-tools)")
        return summary()
    work = tempfile.mkdtemp()
    try:
        bif = compile_ssdt(work, "bif", BIF_BATTERY)
        bix = compile_ssdt(work, "bix", BIX_BATTERY)
        check("iasl compiles both batteries", bif and bix)
        if bif and bix:
            text_mode(work, bif)
            desktop(work, bix)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
