#!/usr/bin/env python3
"""ThreadOS test: an Intel laptop's USB and its Bluetooth, against a model.

No emulator has a 2015 laptop's internal USB bus: an Intel xHCI that
wants scratchpad memory, that the firmware still owns when the system
starts and whose USB 2.0 sockets the firmware left with EHCI - with the
Bluetooth half of an Intel Wireless 3160 in one of them, behind a
webcam, a card reader, a fingerprint reader, a mouse and a
touchscreen. QEMU's xHCI does none of that and has no Bluetooth at all,
so every one of those things worked in the tests and none of them on
the laptop.

So the machine is modelled on the host (tests/hw/xhci_model.c), from
the xHCI specification, Intel's datasheets and the descriptors the
real devices give, and the drivers - kernel/xhci.c, kernel/usbhc.c and
kernel/bt.c, compiled as they are, with the address and
undefined-behaviour sanitizers - run against it. Twice: with
everything there from the start, and with the adapter appearing only
after the walk at start-up, the way a radio does when the firmware
turns it on late.

The kernel images given on the command line are not needed; the test
runs on the host.
"""

import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import check, summary   # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HW = os.path.join(ROOT, "tests", "hw")

DRIVERS = ("xhci.c", "usbhc.c", "bt.c")
HEADERS = ("xhci.h", "usbhc.h", "bt.h", "bt_info.h", "uhci.h", "ehci.h",
           "pci.h")


STUBS = r"""
/* The other two controllers: this machine has its xHCI, and the
   selection layer must never reach for them */
#include <stdint.h>
#include "kernel/include/uhci.h"
#include "kernel/include/ehci.h"
int uhci_init(void) { return 0; }
int uhci_ports(void) { return 0; }
int uhci_port_reset(int p) { (void)p; return 0; }
int uhci_port_lowspeed(int p) { (void)p; return 0; }
int uhci_port_connected(int p) { (void)p; return 0; }
int uhci_control(int a, int l, int m, const void *s, void *d, int n, int i)
{ (void)a; (void)l; (void)m; (void)s; (void)d; (void)n; (void)i; return -1; }
int uhci_interrupt_in(int a, int l, int e, int m, void *d, int n, int *t)
{ (void)a; (void)l; (void)e; (void)m; (void)d; (void)n; (void)t; return -1; }
int ehci_init(void) { return 0; }
const char *ehci_why(void) { return "none"; }
int ehci_ports(void) { return 0; }
int ehci_port_reset(int p) { (void)p; return 0; }
int ehci_port_connected(int p) { (void)p; return 0; }
int ehci_control(int a, int m, const void *s, void *d, int n, int i)
{ (void)a; (void)m; (void)s; (void)d; (void)n; (void)i; return -1; }
int ehci_bulk(int a, int e, int m, void *d, int n, int i, int *t)
{ (void)a; (void)e; (void)m; (void)d; (void)n; (void)i; (void)t; return -1; }
int ehci_int_endpoint(int a, int e, int m) { (void)a; (void)e; (void)m; return -1; }
int ehci_int_in(int h, void *d, int n) { (void)h; (void)d; (void)n; return -1; }
void ehci_int_release(int h) { (void)h; }
"""


def build(work):
    """The drivers in a tree shaped like the repository's, so that their
    own #include lines find the model's headers"""
    kdir = os.path.join(work, "kernel")
    inc = os.path.join(kdir, "include")
    os.makedirs(inc)
    os.makedirs(os.path.join(work, "kernel64"))
    for name in os.listdir(os.path.join(HW, "xhci", "include")):
        shutil.copy(os.path.join(HW, "xhci", "include", name), inc)
    for name in HEADERS:
        shutil.copy(os.path.join(ROOT, "kernel", "include", name), inc)
    shutil.copy(os.path.join(HW, "xhci", "paging64.h"),
                os.path.join(work, "kernel64"))
    for name in DRIVERS:
        shutil.copy(os.path.join(ROOT, "kernel", name), kdir)
    shutil.copy(os.path.join(HW, "xhci_model.c"), work)
    with open(os.path.join(work, "stubs.c"), "w") as fh:
        fh.write(STUBS)
    binary = os.path.join(work, "model")
    out = subprocess.run(
        ["gcc", "-std=gnu11", "-Wall", "-Wextra", "-g", "-O1",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
         "-I", work, "-o", binary]
        + [os.path.join(kdir, n) for n in DRIVERS]
        + [os.path.join(work, "xhci_model.c"), os.path.join(work, "stubs.c")],
        capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr[:4000])
        return None
    return binary


def main():
    print()
    print("--- an Intel laptop's USB and its Bluetooth, against a model ---")
    work = tempfile.mkdtemp()
    try:
        binary = build(work)
        check("the drivers build on the host, with the sanitizers",
              binary is not None)
        if not binary:
            return summary()
        for scenario, name, least in (("boot", "everything there at start-up", 18),
                                      ("late", "the radio switched on late", 5)):
            try:
                out = subprocess.run([binary, scenario], capture_output=True,
                                     text=True, timeout=120)
            except subprocess.TimeoutExpired:
                check(f"{name}: the drivers against the model finish "
                      f"(they hung)", False)
                continue
            text = out.stdout + out.stderr
            lines = [ln for ln in out.stdout.splitlines()
                     if ln.startswith(("ok: ", "FAIL: "))]
            for ln in lines:
                check(f"{name}: {ln.split(': ', 1)[1]}", ln.startswith("ok"))
            sane = ("runtime error" not in text and "Sanitizer" not in text)
            check(f"{name}: every check ran, and nothing the sanitizers saw "
                  f"({len(lines)} checks)",
                  len(lines) >= least and sane and
                  out.returncode in (0, 1))
            if out.returncode != 0 or not sane:
                print(text[-4000:])
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
