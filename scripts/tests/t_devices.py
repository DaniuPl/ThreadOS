#!/usr/bin/env python3
"""ThreadOS smoke test: the Device Manager.

Settings' Devices section and 'devices' in the shell list every device
in the machine and what became of it. The machine here has, besides
QEMU's usual ones, an e1000 (which the network layer takes), an RTL8139
next to it (a card ThreadOS has a driver for but does not use: one card
at a time), QEMU's teaching device 'edu' (which nothing in ThreadOS
drives) and a USB keyboard. Checked:

  the shell's list has exactly the PCI devices QEMU itself reports
  (query-pci), with the same numbers

  edu has no driver; the e1000 works under e1000; the RTL8139 is not
  in use; the host bridge is part of the machine; the USB controller
  works under uhci and the keyboard under usbkbd

  in Settings, the device needing attention is counted and listed
  first, and opening it shows the numbers a driver would be found by

  and the table of what ThreadOS has drivers for (kernel/devices.c)
  has every number the drivers in kernel/ look their devices up by -
  a driver added without a line there would show its spare devices as
  having none

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, _kernel_define

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
_GS = "user/gsettings.c"
LIST_Y = _kernel_define(_GS, "DEV_LIST_Y")
ROW_H = _kernel_define(_GS, "DEV_ROW_H")
STATE_W = _kernel_define(_GS, "DEV_STATE_W")
SEC_DEV = GSET_SECTIONS.index("Devices")
DARK = theme("dark")

LINE = re.compile(r"^\s*pci (\d+):(\d+)\.(\d+)\s+([0-9a-f]{4}):([0-9a-f]{4})"
                  r"\s+(.*?) - ([a-z ]+?)(?: \(([a-z0-9_]+)\))?(?:: (.*))?$")
USB = re.compile(r"^\s*usb socket (\d+)\s+([0-9a-f]{4}):([0-9a-f]{4})"
                 r"\s+(.*?) - ([a-z ]+?)(?: \(([a-z0-9_]+)\))?(?:: (.*))?$")


def driver_numbers():
    """What the drivers in kernel/ look their devices up by: (vendor,
    device) pairs and (class, subclass, interface) triples, the names
    of #defines read from the driver's own source"""
    def value(src, tok):
        tok = tok.strip()
        if re.match(r"^0x[0-9a-fA-F]+$|^\d+$", tok):
            return int(tok, 0)
        # (a vendor is written in hex, which _kernel_define does not read)
        m = re.search(r"#define\s+%s\s+(0x[0-9a-fA-F]+|\d+)" % tok, src)
        return int(m.group(1), 0)
    ids, classes = set(), set()
    kdir = os.path.join(ROOT, "kernel")
    for name in sorted(os.listdir(kdir)):
        if not name.endswith(".c") or name in ("pci.c", "devices.c",
                                                "hwinfo.c"):
            continue
        src = open(os.path.join(kdir, name)).read()
        for a, b in re.findall(r"pci_find_device\(\s*([\w]+)\s*,\s*([\w]+)",
                               src):
            ids.add((value(src, a), value(src, b)))
        for a, b, c in re.findall(
                r"pci_find_class\(\s*([\w]+)\s*,\s*([\w]+)\s*,\s*([\w]+)", src):
            classes.add((value(src, a), value(src, b), value(src, c)))
    return ids, classes


def table_numbers():
    """The lines of the 'known' table in kernel/devices.c"""
    src = open(os.path.join(ROOT, "kernel", "devices.c")).read()
    body = src[src.index("} known[] = {"):]
    body = body[:body.index("};")]
    ids, classes = set(), set()
    for v, d, c, s, p in re.findall(
            r"\{\s*(0x[0-9A-Fa-f]+|0),\s*(0x[0-9A-Fa-f]+|0),\s*"
            r"(0x[0-9A-Fa-f]+|0),\s*(0x[0-9A-Fa-f]+|0),\s*(0x[0-9A-Fa-f]+|0),",
            body):
        v, d, c, s, p = (int(x, 0) for x in (v, d, c, s, p))
        if v:
            ids.add((v, d))
        else:
            classes.add((c, s, p))
    return ids, classes


def qemu_pci(q):
    out = set()
    for bus in q.cmd({"execute": "query-pci"})["return"]:
        for d in bus["devices"]:
            out.add((d["bus"], d["slot"], d["function"],
                     d["id"]["vendor"], d["id"]["device"]))
    return out


def find_settings(q, W, H):
    for slot in range(8):
        x, y = cascade(slot, GSET_W + 2, GSET_H + TITLE_H + 2, w=W, h=H)
        if q.window_title(x, y, GSET_W + 2) == "Settings":
            return x + 1, y + CONTENT_DY
    return None


def main():
    # The static half first: it needs no machine
    want_ids, want_classes = driver_numbers()
    have_ids, have_classes = table_numbers()
    check("the drivers look their devices up by %d numbers and %d classes, "
          "all of them in devices.c's table (missing: %s)"
          % (len(want_ids), len(want_classes),
             sorted((want_ids - have_ids) | (want_classes - have_classes))),
          len(want_ids) >= 8 and len(want_classes) >= 5 and
          want_ids <= have_ids and want_classes <= have_classes)

    slog = tempfile.mktemp(suffix=".log")
    q = Qemu(KERNEL, INITRD, serial=slog, nic="e1000", usbkbd=True,
             devices=["-netdev", "user,id=n1",
                      "-device", "rtl8139,netdev=n1",
                      "-device", "edu"])
    try:
        check("the desktop came up", q.wait_desktop())
        W, H = q.size()
        q.mouse_home()
        truth = qemu_pci(q)
        q.click_desktop()
        out = q.command("devices", "usb socket", timeout=20)
        time.sleep(1)
        out = q.serial()[q.serial().rfind("devices"):]
        pci, usb = {}, []
        for line in out.splitlines():
            m = LINE.match(line)
            if m:
                key = (int(m.group(1)), int(m.group(2)), int(m.group(3)),
                       int(m.group(4), 16), int(m.group(5), 16))
                pci[key] = (m.group(6), m.group(7), m.group(8) or "",
                            m.group(9) or "")
            m = USB.match(line)
            if m:
                usb.append((int(m.group(2), 16), int(m.group(3), 16),
                            m.group(4), m.group(5), m.group(6) or ""))
        check("'devices' lists exactly the %d PCI devices QEMU has "
              "(missing %s, extra %s)" % (len(truth), sorted(truth - set(pci)),
                                          sorted(set(pci) - truth)),
              len(truth) >= 6 and set(pci) == truth)

        def by_id(v, d):
            return next((s for k, s in pci.items() if k[3:] == (v, d)), None)
        edu = by_id(0x1234, 0x11E8)
        check("edu: no driver (%s)" % (edu,),
              edu is not None and edu[1] == "no driver" and edu[2] == "")
        e1000 = by_id(0x8086, 0x100E)
        check("the e1000 works, under e1000 (%s)" % (e1000,),
              e1000 is not None and e1000[1:3] == ("working", "e1000"))
        rtl = by_id(0x10EC, 0x8139)
        check("the RTL8139 beside it is not in use, though there is a "
              "driver for it (%s)" % (rtl,),
              rtl is not None and rtl[1:3] == ("not in use", "rtl8139"))
        host = by_id(0x8086, 0x1237)
        check("the host bridge is part of the machine (%s)" % (host,),
              host is not None and host[1] == "part of the machine")
        uhci = next((s for k, s in pci.items()
                     if k[3:] == (0x8086, 0x7020)), None)
        check("the USB controller works, under uhci (%s)" % (uhci,),
              uhci is not None and uhci[1:3] == ("working", "uhci"))
        kbd = next((u for u in usb if u[2] == "QEMU USB keyboard"), None)
        check("the USB keyboard works, under usbkbd (%s)" % (usb,),
              kbd is not None and kbd[3:] == ("working", "usbkbd"))

        # Settings, Devices
        q.launch_dock("gsettings")
        at = None
        end = time.time() + 15
        while time.time() < end and at is None:
            time.sleep(0.3)
            at = find_settings(q, W, H)
        check("Settings opens", at is not None)
        if at is None:
            return summary()
        cx, cy = at
        q.click(cx + GSET_SIDE_W // 2,
                cy + GSET_SIDE_Y0 + SEC_DEV * GSET_SIDE_H + GSET_SIDE_H // 2)
        q.mouse_goto(W - 20, H // 2)
        head = ""
        end = time.time() + 10
        while time.time() < end and head != "Devices":
            time.sleep(0.5)
            head = q.gset_heading(cx, cy)
        check("its Devices page ('%s')" % head, head == "Devices")
        shot = q.screen_raw()
        n = len(truth) + len(usb)
        counts = (q.gset_info(cx, cy, 0, screen=shot),
                  q.gset_info(cx, cy, 1, screen=shot))
        check("it counts %d devices, one needing attention (%s)"
              % (n, counts), counts == (str(n), "1"))
        top = cy + LIST_Y
        rb = ui_font_baseline_in(UD_PX, top, ROW_H)
        first = q.read_ui(cx + GSET_PANE_X + 14, rb, 200, px=UD_PX,
                          fg=DARK["app_text"], screen=shot)
        state = q.read_ui(cx + GSET_W - 16 - STATE_W + 4, rb, STATE_W,
                          px=UD_PX, fg=DARK["app_dim"], screen=shot)
        check("first on the list, the one with no driver ('%s', '%s')"
              % (first, state),
              first == "QEMU teaching device" and state == "No driver")
        q.click(cx + GSET_PANE_X + 60, top + ROW_H // 2)
        q.mouse_goto(W - 20, H // 2)
        got = ""
        end = time.time() + 10
        while time.time() < end and got != "1234:11e8":
            time.sleep(0.5)
            got = q.gset_info(cx, cy, 1)
        check("opened, it shows the numbers a driver is found by ('%s')"
              % got, got == "1234:11e8")
        check("and that it has none ('%s')" % q.gset_info(cx, cy, 3),
              q.gset_info(cx, cy, 3) == "none")
    finally:
        q.close()
        if os.path.exists(slog):
            os.unlink(slog)
    return summary()


sys.exit(main())
