#!/usr/bin/env python3
"""ThreadOS smoke test: zlib.

zlib 1.4 (third_party/, the release archive unchanged) is built against
musl, and zlibtest uses it: checksums with known values, compress and
uncompress at three levels, zlib, raw and gzip streams through small
buffers, and a .gz file written and read back.

Checked against other implementations:

  the same zlibtest, built on this machine with the same zlib, prints
  the same lines - compressed sizes and the CRC of the compressed bytes
  included, so the deflate output itself is the same, not only what
  inflates from it

  the .gz file zlibtest wrote is read off the disk image (ptest.TfsImage)
  and decompressed by Python's gzip module; it is also byte for byte the
  file the host's build wrote

  a .gz file Python made, fetched over HTTP, is read on ThreadOS and its
  CRC is the one Python computed

Run on its own or through scripts/run_tests.py.
"""

import gzip
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64, TfsImage

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROMPT = "thread64>"
ZLIB_FILES = ("adler32 compress crc32 deflate gzclose gzlib gzread gzwrite "
              "infback inffast inflate inftrees trees uncompr zutil").split()


def host_run(work):
    """zlibtest with the same zlib, built and run on this machine"""
    src = os.path.join(work, "zlib")
    os.makedirs(src)
    subprocess.run(["tar", "-xf", os.path.join(ROOT, "third_party",
                                               "zlib-1.4.tar.xz"),
                    "-C", src, "--strip-components=1"], check=True)
    exe = os.path.join(work, "zlibtest")
    r = subprocess.run(["gcc", "-O2", "-w", "-DHAVE_UNISTD_H", "-I" + src,
                        os.path.join(ROOT, "libc", "tests", "zlibtest.c")]
                       + [os.path.join(src, f + ".c") for f in ZLIB_FILES]
                       + ["-o", exe])
    if r.returncode:
        return None
    run = os.path.join(work, "hostrun")
    os.makedirs(run)
    p = subprocess.run([exe], cwd=run, capture_output=True, text=True,
                       timeout=60)
    with open(os.path.join(run, "zt.gz"), "rb") as fh:
        return p.stdout.splitlines(), fh.read()


def run(q, command, timeout):
    seen = len(q.serial())
    q.type_line(command)
    name = command.split()[1]
    deadline = time.time() + timeout
    while time.time() < deadline:
        if f"[{name}] done, exit code" in q.serial()[seen:]:
            break
        time.sleep(0.5)
    out = q.serial()[seen:]
    m = re.search(r"\[%s\] done, exit code (-?\d+)" % re.escape(name), out)
    lines = [l.strip("\r") for l in out.replace("\r", "").split("\n")]
    body = [l for l in lines[1:] if l and not l.startswith(
            (f"[{name}]", "[kernel]", "[ OK ]", "[ !! ]", PROMPT))]
    return body, int(m.group(1)) if m else None


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    work = tempfile.mkdtemp()
    http_port, http_stop = start_http_server()
    try:
        host = host_run(work)
        check("the same zlib builds and runs on this machine", host is not None)
        if host is None:
            return summary()
        want, host_gz = host
        check("(the host's run is complete: known checksums, three levels, "
              "three kinds of stream, a file)",
              "crc32 of 123456789: cbf43926, adler32: 091e01de" in want and
              len(want) >= 12 and want[-1] == "done")

        rnd = random.Random(7)
        plain = b"".join(b"%d %s\n" % (i, bytes(rnd.choice(b"abcdefgh")
                                                 for _ in range(30)))
                         for i in range(20000))
        SERVED["/py.gz"] = gzip.compress(plain, 9, mtime=0)

        iso = os.path.join(work, "t.iso")
        disk = os.path.join(work, "disk.img")
        with open(disk, "wb") as fh:
            fh.write(b"\0" * DISK_SIZE)
        check("the disc with the kernel builds",
              make_iso64(os.path.join(work, "iso"), iso))
        q = Qemu(KERNEL, INITRD, cdrom=iso, disk=disk, sata=True,
                 serial=os.path.join(work, "serial.log"),
                 qemu="qemu-system-x86_64")
        try:
            q.wait_serial(PROMPT, timeout=90)
            q.wait_serial("] Disk ", timeout=30)
            q.wait_serial("[net] DHCP", timeout=60)
            _, code = run(q, f"run httpget 10.0.2.2 {http_port} /py.gz py.gz",
                          60)
            check("py.gz reaches the ThreadOS disk", code == 0)
            got, code = run(q, "run zlibtest py.gz", 300)
        finally:
            q.close()

        plain_g = [l for l in got if not l.startswith("~")]
        first = next((i for i, (w, g) in enumerate(zip(want, plain_g))
                      if w != g), None)
        where = (f"first difference: host {want[first]!r}, ThreadOS "
                 f"{plain_g[first]!r}" if first is not None else
                 f"host {len(want)} lines, ThreadOS {len(plain_g)}")
        check(f"zlibtest prints exactly what the host's build prints ({where})",
              plain_g == want)
        check(f"and ends with code 0 (got {code})", code == 0)
        tagged = [l for l in got if l.startswith("~py.gz")]
        expect = f"~py.gz: {len(plain)} bytes, crc32 {zlib.crc32(plain):08x}"
        check(f"a file Python's gzip made is read whole ({tagged})",
              tagged == [expect])

        img = TfsImage(disk)
        mine = img.read("/zt.gz")
        check("zt.gz is on the disk", mine is not None)
        if mine is not None:
            try:
                inner = gzip.decompress(mine)
            except Exception as e:      # noqa: BLE001
                inner = repr(e).encode()
            lines = b"".join(b"line %d of a file a different gzip will read\n"
                             % i for i in range(2000))
            check(f"Python's gzip reads it: {len(inner)} bytes, 2000 lines "
                  "and then the data",
                  inner.startswith(lines) and len(inner) == len(lines) + 50000)
            check("and it is the host's file byte for byte "
                  f"({len(mine)} against {len(host_gz)} bytes)",
                  mine == host_gz)
    finally:
        http_stop()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
