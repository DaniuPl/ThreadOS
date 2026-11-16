#!/usr/bin/env python3
"""ThreadOS smoke test: Mbed TLS's own self-test.

Mbed TLS 3.6.7 (third_party/, the release archive unchanged) is built
against musl, and its programs/test/selftest.c runs on ThreadOS as
mbedtest: the library's known-answer tests for every hash, cipher and
mode it has, big-number arithmetic, RSA, elliptic curves, the random
generators built on them - and an entropy test, which draws from the
system (on ThreadOS /dev/urandom, made up by the C library from the
kernel's generator) and fails if any byte of eight draws stays zero.

The same archive is built on this machine, with this machine's C
library, and its selftest has to print exactly what mbedtest prints -
but for the note saying which AES was chosen, which follows the
processor: the guest runs on QEMU's "max" one and has to find AES-NI.
The comparison is not empty: both report every suite they ran, and
every one has to pass.

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

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROMPT = "thread64>"


def makefile_value(name):
    with open(os.path.join(ROOT, "Makefile")) as fh:
        text = fh.read().replace("\\\n", " ")
    return re.search(r"^%s\s*:=\s*(.*)$" % name, text, re.M).group(1).split()


def host_run(work):
    """selftest from the same archive, built and run on this machine"""
    src = os.path.join(work, "mbedtls")
    os.makedirs(src)
    subprocess.run(["tar", "-xf", os.path.join(ROOT, makefile_value(
                    "MBEDTLS_TAR")[0]), "-C", src, "--strip-components=1"],
                   check=True)
    r = subprocess.run(["make", "-s", "-j8", "-C", os.path.join(src, "library"),
                        "CC=gcc", "CFLAGS=-O1", "static"],
                       capture_output=True)
    if r.returncode:
        return None
    lib = os.path.join(src, "library")
    exe = os.path.join(work, "selftest")
    r = subprocess.run(["gcc", "-O1", "-w", "-I" + os.path.join(src, "include"),
                        os.path.join(src, "programs", "test", "selftest.c"),
                        os.path.join(lib, "libmbedtls.a"),
                        os.path.join(lib, "libmbedx509.a"),
                        os.path.join(lib, "libmbedcrypto.a"), "-o", exe])
    if r.returncode:
        return None
    p = subprocess.run([exe], capture_output=True, text=True, timeout=300)
    return [l.strip() for l in p.stdout.splitlines() if l.strip()], p.returncode


def aes_note(lines):
    """Which AES the library chose at run time - AES-NI where the
    processor has it, which depends on the processor, not the system"""
    return [l for l in lines if l.startswith("AES note:")]


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    work = tempfile.mkdtemp()
    try:
        host = host_run(work)
        check("the same Mbed TLS builds and runs its self-test on this "
              "machine", host is not None)
        if host is None:
            return summary()
        want, host_code = host
        suites = re.search(r"Executed (\d+) test suites", "\n".join(want))
        check(f"(the host's run is complete: {suites.group(0) if suites else 'no count'}, "
              f"all passing, the entropy test among them)",
              suites is not None and int(suites.group(1)) >= 25 and
              host_code == 0 and "[ All tests PASS ]" in want and
              "ENTROPY test: passed" in want)

        iso = os.path.join(work, "t.iso")
        check("the disc with the kernel builds",
              make_iso64(os.path.join(work, "iso"), iso))
        # The processor with every instruction QEMU can do: the AES-NI
        # path runs too, and keeps its XMM registers across switches
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=os.path.join(work, "s.log"),
                 qemu="qemu-system-x86_64", cpu_model="max")
        try:
            q.wait_serial(PROMPT, timeout=90)
            seen = len(q.serial())
            q.type_line("run mbedtest")
            deadline = time.time() + 600
            while time.time() < deadline:
                if "[mbedtest] done, exit code" in q.serial()[seen:]:
                    break
                time.sleep(0.5)
            out = q.serial()[seen:].replace("\r", "")
        finally:
            q.close()
        m = re.search(r"\[mbedtest\] done, exit code (-?\d+)", out)
        got = [l.strip() for l in out.split("\n")[1:] if l.strip() and
               not l.startswith(("[mbedtest]", "[kernel]", "[ OK ]",
                                 "[ !! ]", PROMPT))]
        check(f"on QEMU's 'max' processor the library finds AES-NI "
              f"({aes_note(got)})",
              "AES note: using AESNI." in aes_note(got))
        want = [l for l in want if not l.startswith("AES note:")]
        got = [l for l in got if not l.startswith("AES note:")]
        first = next((i for i, (w, g) in enumerate(zip(want, got))
                      if w != g), None)
        where = (f"first difference: host {want[first]!r}, ThreadOS "
                 f"{got[first]!r}" if first is not None else
                 f"host {len(want)} lines, ThreadOS {len(got)}")
        check(f"mbedtest prints exactly what the host's selftest prints "
              f"({where})", got == want)
        fails = [l.strip() for l in got if "fail" in l.lower()]
        check(f"no suite fails on ThreadOS ({fails[:3]})", not fails and
              "ENTROPY test: passed" in got)
        check(f"and it ends with code 0 (got {m.group(1) if m else None})",
              m is not None and m.group(1) == "0")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
