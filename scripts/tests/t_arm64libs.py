#!/usr/bin/env python3
"""ThreadOS smoke test: WebKit's libraries on the ARM64 kernel.

What Weft is built on - GLib with libffi and PCRE2, libgcrypt and
libgpg-error, libtasn1, libwebp, libxkbcommon, HarfBuzz with ICU,
libepoxy - built for aarch64 by the PC's own rules (the Makefile's
TARGET=aarch64, into BUILD/aarch64: `make arm64-libs`), run on the
ARM64 kernel and held to the same answers as on the PC:

  glibtest (libc/tests/glibtest.c, --no-disk: the ARM64 kernel has no
  disk to write yet) prints exactly what it prints built against this
  machine's GLib, on one core and on four: the main loop, pipes and
  socket pairs watched, threads and pools, GObject's signals through
  libffi, regular expressions, GVariant, key files, GIO's streams, and
  processes spawned with pipes and descriptors as WebKit spawns its own

  wpedeps (libc/tests/wpedeps.c) answers t_wpedeps.py's questions -
  hashes, ciphers, signatures and keys against the cryptography
  package, pictures against Pillow's libwebp, keymaps against their
  own definitions, shaping against the font's - every answer as the
  other implementation has it; its files come in the initrd
"""

import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, noarm64, arm64_boot, KERNEL
from t_glib import host_glibtest
import t_wpedeps

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AARCH64 = os.path.join(os.path.dirname(os.path.abspath(KERNEL)), "aarch64",
                       "user64")


def initrd(work, files):
    """An initrd of these files (name: path or bytes) for the kernel"""
    specs = []
    for name, what in files.items():
        if isinstance(what, bytes):
            path = os.path.join(work, name)
            with open(path, "wb") as fh:
                fh.write(what)
            what = path
        specs.append(f"{name}={what}")
    img = os.path.join(work, "initrd.img")
    subprocess.run([sys.executable, os.path.join(ROOT, "scripts",
                                                 "mkinitrd.py"), img] + specs,
                   check=True, capture_output=True)
    return img


def asn_defs():
    """wpedeps' ASN.1 definitions, as its source has them: libtasn1 reads
    them from a file, which on the PC wpedeps writes itself"""
    with open(os.path.join(ROOT, "libc", "tests", "wpedeps.c")) as fh:
        src = fh.read()
    body = src.split("static const char ASN_DEFS[] =", 1)[1].split(";", 1)[0]
    text = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', body))
    return text.encode().decode("unicode_escape").encode()


def program(said, name):
    """What the program printed, and the code it ended with"""
    start = re.search(r"^\[proc\] %s started, pid \d+$" % name, said, re.M)
    end = re.search(r"^\[proc\] %s \(pid \d+\) ended with code (-?\d+)$"
                    % name, said, re.M)
    body = said[start.end():end.start()] if start and end else ""
    return ([l for l in body.split("\n") if l and not l.startswith("[")],
            int(end.group(1)) if end else None, body)


def glib(work):
    host = host_glibtest(work, ["--no-disk"])
    check("glibtest builds and runs against this machine's GLib",
          host is not None and host[1] == 0)
    if host is None:
        return
    want = [l for l in host[0] if not l.startswith("~")]
    img = initrd(work, {"glibtest": os.path.join(AARCH64, "glibtest.elf")})
    for cores in (1, 4):
        said = arm64_boot(cores=cores, mib=1024, initrd=img, timeout=300,
                          append="run=glibtest,--no-disk")
        got, code, _ = program(said, "glibtest")
        plain = [l for l in got if not l.startswith("~")]
        first = next((i for i, (w, g) in enumerate(zip(want, plain))
                      if w != g), None)
        where = (f"first difference at line {first + 1}: host "
                 f"{want[first]!r}, ThreadOS {plain[first]!r}"
                 if first is not None else
                 f"host {len(want)} lines, ThreadOS {len(plain)}")
        check(f"({cores} core(s)) glibtest prints exactly what it prints "
              f"against this machine's GLib ({where})", plain == want)
        check(f"({cores} core(s)) and ends with code 0 ({code})", code == 0)


def deps(work):
    try:
        from PIL import features
        have = bool(features.check("webp"))
        import cryptography     # noqa: F401
        import fontTools        # noqa: F401
    except ImportError:
        have = False
    check("Pillow with WebP, the cryptography package and fontTools are "
          "here to compare with", have)
    if not have:
        return
    qs = t_wpedeps.questions()
    files = dict(qs.files)
    files["wpedeps"] = os.path.join(AARCH64, "wpedeps.elf")
    files["wpedeps.asn"] = asn_defs()
    img = initrd(work, files)
    said = arm64_boot(cores=2, mib=1024, initrd=img, timeout=900,
                      append="run=wpedeps,questions.txt")
    _, code, body = program(said, "wpedeps")
    got = t_wpedeps.answers(body)
    check(f"wpedeps answers all {len(qs.lines)} questions ({len(got)})",
          len(got) == len(qs.lines) and
          f"wpedeps: {len(qs.lines)} questions answered" in body)
    t_wpedeps.judge(qs, got)
    check(f"and ends with code 0 ({code})", code == 0)


def main():
    print("--- WebKit's libraries on the ARM64 kernel ---")
    why = noarm64()
    if why:
        print("SKIPPED: " + why)
        return summary()
    if not all(os.path.exists(os.path.join(AARCH64, p))
               for p in ("glibtest.elf", "wpedeps.elf")):
        print("SKIPPED: no aarch64 build of the libraries (make arm64-libs)")
        return summary()
    work = tempfile.mkdtemp()
    try:
        glib(work)
        deps(work)
    finally:
        subprocess.run(["rm", "-rf", work])
    return summary()


if __name__ == "__main__":
    sys.exit(main())
