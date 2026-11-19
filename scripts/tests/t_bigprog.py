#!/usr/bin/env python3
"""ThreadOS smoke test: a program larger than 2 MiB, run from the disk
(64-bit kernel).

A program on the disk used to be read into the kernel's heap whole
before it was loaded, and was refused above 2 MiB; WebKit's processes
are tens of megabytes. The kernel now reads a larger one straight into
the program's own pages as it loads it (kernel64/elf64.c,
elf64_load_file).

libc/tests/bigprog.c is 10 MiB: 5 MiB of known read-only bytes, 1 MiB
of known writable ones and 3 MiB of zeroed memory, the bytes made by
scripts/mkblob.py. It says the SHA-256 of the first and of the second
with every byte inverted, and whether the third is zero. The test makes
the same bytes here and holds the answers to hashlib's - a page read
from the wrong place in the file, or left out, changes a hash. The
program is fetched onto the ThreadOS disk over HTTP and run from there
twice, by the shell (a new process) and by execv (a process replacing
itself), which are the kernel's two ways of loading a program.

And from an application package (.tapp, scripts/mktapp.py): an image
in the initrd's format which the loader hands over beside the initrd
under the name "tapp" (Weft's programs come so). A disc whose GRUB
brings one with bigprog in it boots, says it took the package, and
runs bigprog from it with the same answers - the initrd itself does not
have it.

Run on its own or through scripts/run_tests.py.
"""

import hashlib
import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64
from mkblob import blob
import subprocess

PROMPT = "thread64>"
BUILD = os.path.dirname(os.path.abspath(KERNEL64 or "build/x"))
ELF = os.path.join(BUILD, "user64", "bigprog.elf")
RO = blob("ro", 5 << 20)
RW = bytes(b ^ 0xFF for b in blob("rw", 1 << 20))
BSS = 3 << 20


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
    m = re.search(r"\[%s\] done, exit code (-?\d+)" % re.escape(name), out)
    # Only what follows the shell's echo of the command: the prompt the
    # test waited for at boot can be another console's, printed while
    # the first one is still writing out its command list, and the tail
    # of that list would otherwise be read as the program's output
    text = out.replace("\r", "")
    cut = text.find(command + "\n")
    lines = (text[cut + len(command) + 1:].split("\n") if cut >= 0
             else text.split("\n")[1:])
    body = [l for l in lines if l and not l.startswith(
            (f"[{name}]", "[kernel]", "[ OK ]", "[ !! ]", PROMPT,
             "ThreadOS 64 - console"))]
    return body, int(m.group(1)) if m else None


def answers():
    return [f"rodata {len(RO)} {hashlib.sha256(RO).hexdigest()}",
            f"data {len(RW)} {hashlib.sha256(RW).hexdigest()}",
            f"bss {BSS} zero",
            f"bss written {0x5A}"]


def from_apps_image(work, want):
    """bigprog from an application package, on a disc whose GRUB hands
    it over as a module named "tapp" """
    apps = os.path.join(work, "bigprog.tapp")
    root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    made = subprocess.run([sys.executable,
                           os.path.join(root, "scripts", "mktapp.py"), apps,
                           "--id", "bigprog", "--name", "Bigprog",
                           "--version", "1", "--arch", "x86_64",
                           "--exec", "bigprog", "bigprog=" + ELF],
                          capture_output=True).returncode == 0
    iso = os.path.join(work, "apps.iso")
    check("a package with bigprog in it, and a disc that brings it "
          "beside the initrd",
          made and make_iso64(os.path.join(work, "isoapps"), iso,
                              tapps=[apps]))
    q = Qemu(KERNEL, INITRD, cdrom=iso, serial=os.path.join(work, "a.log"),
             qemu="qemu-system-x86_64", ram_mib=512)
    try:
        q.wait_serial(PROMPT, timeout=90)
        log = q.serial()
        m = re.search(r"Package bigprog 1: Bigprog, (\d+) KiB", log)
        check(f"the kernel takes the package the loader named 'tapp' "
              f"({m.group(0) if m else 'nothing said'})",
              m is not None and
              int(m.group(1)) == os.path.getsize(apps) // 1024)
        got, code = run(q, "run bigprog", timeout=120)
        check(f"bigprog runs from it and ends with code 0 (got {code})",
              code == 0 and got[-1:] == ["done"])
        check(f"with the bytes it was built with: {got[:len(want)]} "
              "against hashlib", got[:len(want)] == want)
    finally:
        q.close()


def main():
    print()
    print("--- A program larger than 2 MiB from the disk (64-bit kernel) ---")
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    if not os.path.exists(ELF):
        print("SKIPPED: no bigprog - make")
        return summary()
    size = os.path.getsize(ELF)
    check(f"the program is larger than the 2 MiB a program from the disk "
          f"used to be held to ({size} B)", size > 4 << 20)

    http_port, http_stop = start_http_server()
    with open(ELF, "rb") as fh:
        SERVED["/bigprog"] = fh.read()
    work = tempfile.mkdtemp()
    iso = os.path.join(work, "t.iso")
    disk = os.path.join(work, "disk.img")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)
    want = answers()
    try:
        check("the disc with the 64-bit kernel builds",
              make_iso64(os.path.join(work, "iso"), iso))
        q = Qemu(KERNEL, INITRD, cdrom=iso, disk=disk, sata=True,
                 serial=os.path.join(work, "serial.log"),
                 qemu="qemu-system-x86_64", ram_mib=512)
        try:
            q.wait_serial(PROMPT, timeout=90)
            q.wait_serial("[net] DHCP", timeout=60)
            _, code = run(q, f"run httpget 10.0.2.2 {http_port} "
                             "/bigprog bigprog", timeout=180)
            check("the program reaches the ThreadOS disk over HTTP",
                  code == 0)

            got, code = run(q, "run bigprog", timeout=120)
            check("started by the shell, it loads, runs and ends with "
                  f"code 0 (got {code}; {got[-1:] if got else 'nothing'})",
                  code == 0 and got[-1:] == ["done"])
            for w in want:
                key = w.split()[0] + (" " + w.split()[1]
                                      if w.startswith("bss") else "")
                g = next((l for l in got if l.startswith(key)), None)
                check(f"{w.split()[0]}: ThreadOS says {g!r}, hashlib "
                      f"{w!r}", g == w)

            got, code = run(q, "run bigprog exec", timeout=120)
            check("replacing itself with execv, it loads a second time "
                  f"and ends with code 0 (got {code})",
                  code == 0 and "exec" in got and got[-1:] == ["done"])
            after = got[got.index("exec") + 1:] if "exec" in got else []
            check("and the second image has the same bytes: "
                  f"{after[:len(want)]} against hashlib",
                  after[:len(want)] == want)
        finally:
            q.close()
        from_apps_image(work, want)
    finally:
        http_stop()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
