#!/usr/bin/env python3
"""ThreadOS smoke test: devices where a laptop has them.

In QEMU every device sits on PCI bus 0. On a laptop only the chipset's
own controllers do: the Ethernet card, the Wi-Fi and an NVMe disk each
sit on a bus of their own behind a PCI Express root port - a bridge -
and a laptop has two HD Audio controllers, the first of them the
graphics chip's, with nothing on it but the HDMI socket. The drivers
looked on bus 0 alone and took the first HD Audio controller they
found: on a 2014 Lenovo the Device Manager showed the RTL8168 and the
Intel 3160 as cards nothing used, and the sound as failed, with the
controller that has the speakers on it never tried.

The machine here is built the same way: an e1000 on bus 1 behind a
PCI-to-PCI bridge, and two HD Audio controllers on bus 0 - the first
with no codec at all, the second with one whose sound QEMU writes to a
file. Checked:

  the network card behind the bridge is found and DHCP gives it its
  address, and the host answers a ping through it

  the sound comes out of the second controller - a 440 Hz tone of 200
  ms read back from the file - and the system says which codec it took

  'devices' lists every device QEMU reports (query-pci), bridge and
  bus 1 included; the e1000 on bus 1 works under e1000, the second
  controller works under hda, and the first is not in use - with the
  reason, that no codec answered - rather than failed

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, no64, wav_tone

LINE = re.compile(r"^\s*pci (\d+):(\d+)\.(\d+)\s+([0-9a-f]{4}):([0-9a-f]{4})"
                  r"\s+(.*?) - ([a-z ]+?)(?: \(([a-z0-9_]+)\))?(?:: (.*))?$")

BRIDGE_SLOT = 0x10
NIC_SLOT = 0x01
HDA_EMPTY, HDA_SOUND = 0x0E, 0x0F


def qemu_pci(q):
    """Every device QEMU has, the ones behind bridges too:
    (bus, slot, function) -> (vendor, device)"""
    out = {}

    def walk(devices):
        for d in devices:
            out[(d["bus"], d["slot"], d["function"])] = (
                d["id"]["vendor"], d["id"]["device"])
            walk(d.get("pci_bridge", {}).get("devices", []))
    for bus in q.cmd({"execute": "query-pci"})["return"]:
        walk(bus["devices"])
    return out


def main():
    print()
    print("--- devices behind a bridge, and two sound controllers ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()

    wav = tempfile.mktemp(suffix=".wav")
    slog = tempfile.mktemp(suffix=".log")
    q = Qemu(KERNEL, INITRD, append=TEXT_MODE, serial=slog, nic=None,
             devices=["-device", "pci-bridge,id=br1,chassis_nr=1,addr=0x%x"
                      % BRIDGE_SLOT,
                      "-device", "e1000,netdev=n0,bus=br1,addr=0x%x" % NIC_SLOT,
                      "-audiodev", "wav,id=snd0,path=" + wav,
                      "-device", "intel-hda,id=hda0,addr=0x%x" % HDA_EMPTY,
                      "-device", "intel-hda,id=hda1,addr=0x%x" % HDA_SOUND,
                      "-device", "hda-output,bus=hda1.0,audiodev=snd0"])
    try:
        out = q.wait_serial("thread64>", timeout=90)
        truth = qemu_pci(q)
        on_bus1 = {k: v for k, v in truth.items() if k[0] == 1}
        check("QEMU put the e1000 on bus 1, behind the bridge (%s)"
              % sorted(on_bus1.items()),
              on_bus1 == {(1, NIC_SLOT, 0): (0x8086, 0x100E)})

        # --- the network card
        try:
            out = q.wait_serial("DHCP: granted 10.0.2.15", timeout=60)
        except RuntimeError:
            out = q.serial()                # (no card found: said below)
        check("the network card behind the bridge is found (e1000)",
              "Network: e1000" in out)
        check("and DHCP gives it its address through the bridge",
              "DHCP: granted 10.0.2.15" in out)
        out = q.command("ping 10.0.2.2", "--- statistics", timeout=40)
        m = re.search(r"sent (\d+), received (\d+)", out)
        check("and the host answers a ping through it (%s of %s)"
              % (m.group(2) if m else "-", m.group(1) if m else "-"),
              m is not None and int(m.group(2)) > 0)

        # --- the sound
        try:
            out = q.wait_serial("The 64-bit system is ready", timeout=60)
        except RuntimeError:
            out = q.serial()
        check("the system takes a sound controller with a codec on it",
              "Sound: Intel HDA" in out)
        before = os.path.getsize(wav) - 44 if os.path.exists(wav) else 0
        out = q.command("beep 440 200", "beep:", timeout=30)
        check("'beep' does not refuse", "there is no sound" not in out)
        tone = None
        for _ in range(20):
            tone = wav_tone(wav, max(before, 0)) if os.path.exists(wav) \
                else None
            if tone and tone[1] >= 150:
                break
            q.wait_serial("thread64>", timeout=1)
        check("the tone comes out of the second controller's codec",
              tone is not None)
        if tone:
            peak, ms, hz = tone
            check("at 440 Hz (%.0f Hz), for 200 ms (%.0f ms)" % (hz, ms),
                  420 <= hz <= 460 and 150 <= ms <= 260)

        # --- what the Device Manager makes of it
        # The shell runs one command after the other: when 'whoami' has
        # answered, the list before it is whole
        start = len(q.serial())
        q.type_line("devices")
        q.command("whoami", "guest", timeout=30)
        pci = {}
        for line in q.serial()[start:].splitlines():
            m = LINE.match(line)
            if m:
                pci[(int(m.group(1)), int(m.group(2)), int(m.group(3)))] = (
                    int(m.group(4), 16), int(m.group(5), 16), m.group(7),
                    m.group(8) or "", m.group(9) or "")
        listed = {k: v[:2] for k, v in pci.items()}
        check("'devices' lists every device QEMU has, bus 1 included "
              "(missing %s, extra %s)"
              % (sorted(set(truth) - set(listed)),
                 sorted(set(listed) - set(truth))),
              listed == truth and (1, NIC_SLOT, 0) in listed)
        nic = pci.get((1, NIC_SLOT, 0))
        check("the e1000 on bus 1 works, under e1000 (%s)" % (nic,),
              nic is not None and nic[2:4] == ("working", "e1000"))
        good = pci.get((0, HDA_SOUND, 0))
        check("the controller with the codec works, under hda (%s)"
              % (good,), good is not None and good[2:4] == ("working", "hda"))
        empty = pci.get((0, HDA_EMPTY, 0))
        check("the one without is not in use - not failed - and says why "
              "(%s)" % (empty,),
              empty is not None and empty[2:4] == ("not in use", "hda")
              and "no codec answered" in empty[4])
    finally:
        q.close()
        for f in (wav, slog):
            if os.path.exists(f):
                os.unlink(f)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
