#!/usr/bin/env python3
"""ThreadOS smoke test: C++ on ThreadOS, and litehtml laying out pages
(64-bit kernel).

A browser engine is the largest piece of other people's code this system
has run, and the first in C++: litehtml (third_party/litehtml) parses
HTML and CSS and works out where every box and every word goes. Under
it is a C++ runtime put together for ThreadOS - the host compiler's
libstdc++ on musl, without exceptions (libc/cxx). Strings, maps, lists,
shared pointers, virtual calls, dynamic_cast and floating point in
every box: if any of it misbehaves, the boxes land somewhere else.

So the check is a comparison, from outside:

  apps/browser/layouttest.cpp lays out four pages - blocks with
  collapsing margins and wrapping text, floats, lists and a table,
  flexbox with selectors and relative positioning - with a made-up font
  whose widths are known exactly, and prints every box and every word
  with its position

  the same source and the same library are built for this machine with
  its own C++ library; the ThreadOS build is fetched onto the ThreadOS
  disk over HTTP and run from there (programs run from the disk as well
  as from the initrd), and the two outputs have to agree line for line

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
from ptest import check, summary, make_iso64, no64

PROMPT = "thread64>"
BUILD = os.path.dirname(os.path.abspath(KERNEL64 or "build/x"))
ELF = os.path.join(BUILD, "user64", "layouttest.elf")
HOST = os.path.join(BUILD, "litehtml", "host", "layouttest")


def wait_after(q, seen, text, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if text in q.serial()[seen:]:
            break
        time.sleep(0.5)
    return q.serial()[seen:]


def run(q, command, timeout=120):
    """A program from the kernel's shell: the lines it printed, its exit
    code"""
    seen = len(q.serial())
    q.type_line(command)
    name = command.split()[1]
    out = wait_after(q, seen, f"[{name}] done, exit code", timeout)
    out = out.encode("latin1").decode("utf-8", "replace")
    m = re.search(r"\[%s\] done, exit code (-?\d+)" % re.escape(name), out)
    lines = [l.strip("\r") for l in out.replace("\r", "").split("\n")]
    body = [l for l in lines[1:] if l and not l.startswith(
            (f"[{name}]", "[kernel]", "[ OK ]", "[ !! ]", PROMPT))]
    return body, int(m.group(1)) if m else None


def main():
    print()
    print("--- C++ and litehtml (the 64-bit kernel) ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    if not os.path.exists(ELF) or not os.path.exists(HOST):
        print("SKIPPED: no layouttest - make browser")
        return summary()

    want = subprocess.run([HOST], capture_output=True, text=True,
                          timeout=60).stdout.splitlines()
    check("the layout test runs on this machine and lays out all four "
          f"pages ({len(want)} lines)",
          len(want) > 150 and want[-1] == "done" and
          sum(l.startswith("== page") for l in want) == 4)

    http_port, http_stop = start_http_server()
    with open(ELF, "rb") as fh:
        SERVED["/layouttest"] = fh.read()
    work = tempfile.mkdtemp()
    iso = os.path.join(work, "t.iso")
    disk = os.path.join(work, "disk.img")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    try:
        check("the disc with the 64-bit kernel builds",
              make_iso64(os.path.join(work, "iso"), iso))
        q = Qemu(KERNEL, INITRD, cdrom=iso, disk=disk, sata=True,
                 serial=os.path.join(work, "serial.log"),
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial(PROMPT, timeout=90)
            q.wait_serial("[net] DHCP", timeout=60)
            _, code = run(q, f"run httpget 10.0.2.2 {http_port} "
                             "/layouttest layouttest")
            check("the program reaches the ThreadOS disk over HTTP",
                  code == 0)

            got, code = run(q, "run layouttest", timeout=180)
            first = next((i for i, (w, g) in enumerate(zip(want, got))
                          if w != g), None)
            where = (f"first difference at line {first + 1}: this machine "
                     f"{want[first]!r}, ThreadOS {got[first]!r}"
                     if first is not None else
                     f"this machine {len(want)} lines, ThreadOS {len(got)}")
            check("litehtml on ThreadOS lays the pages out exactly as on "
                  f"this machine, every box and word ({where})", got == want)
            check(f"and the program ends with code 0 (got {code})", code == 0)

            # What the comparison covered - so that agreeing is not
            # agreeing about nothing
            text = [l for l in got if l.startswith("text ")]
            check("the page title came through the parser "
                  f"({[l for l in got if l.startswith('title')][:1]})",
                  'title "Blocks"' in got)
            check(f"words were placed ({len(text)}), on more than one line "
                  "of each paragraph",
                  len(text) > 60 and
                  len({l.split()[1].split(",")[1] for l in text}) > 10)
            check("a float pushed text aside: the first line starts right "
                  "of the 100-pixel float",
                  any(l.startswith("text 100.00,") for l in got))
            check("the table was laid out, the bullets drawn and the "
                  "numbered list counted from its start=3",
                  any(l.strip().startswith("table ") for l in got) and
                  sum(l.startswith("marker ") for l in got) == 2 and
                  any(l.endswith('"3."') for l in got) and
                  any(l.endswith('"4."') for l in got))
            check("flexbox spread the row, and text-transform reached the "
                  "container", any('"FLEX"' in l for l in got))
        finally:
            q.close()
    finally:
        http_stop()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
