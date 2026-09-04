#!/usr/bin/env python3
"""Checks the ProtonOS parsers against corrupted data.

Two fronts: the ring 3 programs (certificates, HTML, HTTP) and the
packet path in the kernel (net.c + tcp.c). In the kernel an error does
not end in the death of a process but in the system going down, so it
is all the more worth looking for.

Certificates, HTML and HTTP responses come straight from the network -
anybody can supply them. This script compiles the parsers on the host
with the address and undefined-behaviour sanitizers, and then feeds them
thousands of inputs made by corrupting valid data: truncating, moving
bytes around, writing extreme lengths, cutting fragments out.

Every crash of a program (or a sanitizer report) is a bug to be fixed -
we then save the input to a file, so that it can be repeated.

Usage: python3 scripts/fuzz_parsers.py [number_of_cases]
"""

import os
import random
import shutil
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FUZZ = os.path.join(ROOT, "tests", "fuzz")
CASES = int(sys.argv[1]) if len(sys.argv) > 1 else 400


def build(workdir):
    """Compiles the harness. The userspace headers pull in 'uapi.h', and
    that one picks the entry into the kernel appropriate for the
    processor mode - on the host neither of them makes sense. So we
    substitute a stub: a directory of symlinks to the real headers, and
    instead of the entry into the kernel a file with types and small
    details only.

    We stub 'usys.h' under its own name too - the parsers should no
    longer call it directly, but if one of them went back to the old
    habit, better it hit a stub than a syscall."""
    inc = os.path.join(workdir, "include")
    os.makedirs(inc, exist_ok=True)

    stubbed = ("usys.h", "uapi.h")
    for name in os.listdir(os.path.join(ROOT, "user")):
        if name.endswith(".h") and name not in stubbed:
            os.symlink(os.path.join(ROOT, "user", name),
                       os.path.join(inc, name))
    for name in stubbed:
        shutil.copy(os.path.join(FUZZ, "stub_usys.h"),
                    os.path.join(inc, name))

    binary = os.path.join(workdir, "harness")
    cmd = ["gcc", "-I", inc, "-fsanitize=address,undefined",
           "-fno-omit-frame-pointer", "-g", "-O1", "-Wall",
           "-o", binary, os.path.join(FUZZ, "harness.c")]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr[:4000])
        raise SystemExit("nie udalo sie zbudowac harnessu")
    return binary


def build_kernel(workdir):
    """Compiles net.c and tcp.c with hardware stubs - the kernel's sources
    are symlinked in kstub/, next to the directory of substituted
    headers."""
    kstub = os.path.join(FUZZ, "kstub")
    inc = os.path.join(kstub, "include")
    objs = []

    for src in ("net.c", "tcp.c"):
        obj = os.path.join(workdir, src.replace(".c", ".o"))
        cmd = ["gcc", "-I", inc, "-fsanitize=address,undefined",
               "-fno-omit-frame-pointer", "-g", "-O1", "-Wall",
               "-c", os.path.join(kstub, src), "-o", obj]
        out = subprocess.run(cmd, capture_output=True, text=True)
        if out.returncode != 0:
            print(out.stderr[:3000])
            raise SystemExit("nie udalo sie zbudowac %s" % src)
        objs.append(obj)

    binary = os.path.join(workdir, "kharness")
    cmd = ["gcc", "-I", inc, "-fsanitize=address,undefined", "-g", "-O1",
           "-Wall", "-o", binary,
           os.path.join(FUZZ, "kernel_harness.c")] + objs
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr[:3000])
        raise SystemExit("nie udalo sie zbudowac harnessu jadra")
    return binary


def frames():
    """Valid Ethernet frames: ARP, ICMP, UDP and TCP."""
    mac_us = bytes([0x52, 0x54, 0x00, 0x12, 0x34, 0x56])
    mac_them = bytes([0x52, 0x55, 0x0a, 0x00, 0x02, 0x02])
    ip_us = bytes([10, 0, 2, 15])
    ip_them = bytes([10, 0, 2, 2])

    def eth(kind, payload):
        return mac_us + mac_them + kind + payload

    def ip(proto, payload):
        total = 20 + len(payload)
        hdr = (bytes([0x45, 0x00]) + total.to_bytes(2, "big") +
               b"\x00\x01\x00\x00\x40" + bytes([proto]) + b"\x00\x00" +
               ip_them + ip_us)
        return eth(b"\x08\x00", hdr + payload)

    out = []

    arp = (b"\x00\x01\x08\x00\x06\x04\x00\x01" + mac_them + ip_them +
           b"\x00" * 6 + ip_us)
    out.append(eth(b"\x08\x06", arp))

    icmp = b"\x08\x00\x00\x00\x12\x34\x00\x01" + b"proton" * 4
    out.append(ip(1, icmp))

    udp_payload = b"zapytanie testowe"
    udp = (b"\x30\x39" + (53).to_bytes(2, "big") +
           (8 + len(udp_payload)).to_bytes(2, "big") + b"\x00\x00" +
           udp_payload)
    out.append(ip(17, udp))

    tcp = (b"\x30\x39" + (5555).to_bytes(2, "big") +
           b"\x00\x00\x00\x01\x00\x00\x00\x00" +
           b"\x50\x02\xff\xff\x00\x00\x00\x00")
    out.append(ip(6, tcp))
    return out


def seeds(workdir):
    """Valid output data: certificates (RSA and two curves), a signature,
    an HTML page, an HTTP response, deflate streams and a JPEG."""
    def openssl(*args, **kw):
        subprocess.run(["openssl"] + list(args), check=True,
                       capture_output=True, timeout=120, **kw)

    j = lambda n: os.path.join(workdir, n)
    out = {"x509": [], "ecdsa": [], "html": [], "http": [],
           "inflate": [], "jpeg": []}

    for tag, keyargs in [("rsa", ["-newkey", "rsa:2048"]),
                         ("p256", ["-newkey", "ec", "-pkeyopt",
                                   "ec_paramgen_curve:prime256v1"]),
                         ("p384", ["-newkey", "ec", "-pkeyopt",
                                   "ec_paramgen_curve:secp384r1", "-sha384"])]:
        openssl("req", "-x509", *keyargs, "-nodes", "-keyout", j(tag + ".key"),
                "-out", j(tag + ".pem"), "-days", "3650",
                "-subj", "/CN=protonos.test",
                "-addext", "subjectAltName=DNS:protonos.test,DNS:*.test")
        openssl("x509", "-in", j(tag + ".pem"), "-outform", "DER",
                "-out", j(tag + ".der"))
        out["x509"].append(open(j(tag + ".der"), "rb").read())

    with open(j("msg"), "wb") as f:
        f.write(b"ProtonOS")
    openssl("dgst", "-sha256", "-sign", j("p256.key"), "-out", j("p256.sig"),
            j("msg"))
    out["ecdsa"].append(open(j("p256.sig"), "rb").read())

    out["html"].append(
        b"<html><head><title>x</title><style>a{}</style></head><body>"
        b"<h1>Naglowek</h1><p>Tekst z encja &amp; i odnosnikiem "
        b'<a href="/dalej">tutaj</a>.</p><ul><li>raz</li><li>dwa</li></ul>'
        b"<script>var x=1;</script></body></html>")
    out["html"].append(b"<p>" + b"slowo " * 500 + b"</p>")

    out["http"].append(
        b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
        b"Transfer-Encoding: chunked\r\nLocation: /gdzies\r\n\r\n")

    # The picture decoders. A deflate stream at three levels, because
    # level zero writes stored blocks and the other two write both
    # kinds of tree; and the test harness's own JPEG, which came from
    # an outside encoder.
    body = b"ProtonOS: obrazek, ktory da sie spakowac. " * 40
    for level in (0, 6, 9):
        out["inflate"].append(zlib.compress(body, level))
    out["inflate"].append(zlib.compress(bytes(range(256)) * 8, 9))
    raw = zlib.compressobj(9, zlib.DEFLATED, -15)   # no zlib wrapper
    out["inflate"].append(raw.compress(body) + raw.flush())

    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    from ptest import JPEG_IMAGE           # noqa: E402 - it lives there
    out["jpeg"].append(JPEG_IMAGE)
    return out


def mutate(data, rng):
    """Corrupting the data: truncation, byte changes, insertions, extreme
    lengths."""
    b = bytearray(data)
    how = rng.randrange(6)

    if how == 0 and len(b) > 1:                    # truncation
        del b[rng.randrange(1, len(b)):]
    elif how == 1 and b:                           # single bytes
        for _ in range(rng.randrange(1, 8)):
            b[rng.randrange(len(b))] = rng.randrange(256)
    elif how == 2 and b:                           # extreme values
        for _ in range(rng.randrange(1, 5)):
            b[rng.randrange(len(b))] = rng.choice([0x00, 0x01, 0x7f, 0x80,
                                                   0xfe, 0xff, 0x30, 0x82])
    elif how == 3 and len(b) > 4:                  # cutting a fragment out
        i = rng.randrange(len(b) - 2)
        del b[i:i + rng.randrange(1, min(64, len(b) - i))]
    elif how == 4:                                 # rubbish appended
        b += bytes(rng.randrange(256) for _ in range(rng.randrange(1, 64)))
    else:                                          # a fragment repeated
        if len(b) > 8:
            i = rng.randrange(len(b) - 4)
            b[i:i] = b[i:i + rng.randrange(1, 32)]
    return bytes(b)


def main():
    rng = random.Random(20260826)
    workdir = tempfile.mkdtemp(prefix="protonos-fuzz-")
    failures = []

    try:
        binary = build(workdir)
        kbinary = build_kernel(workdir)
        corpus = seeds(workdir)
        corpus["ramki"] = frames()
        print("harness zbudowany, przypadkow na parser: %d" % CASES)

        for parser, base in corpus.items():
            crashes = 0
            for i in range(CASES):
                data = mutate(rng.choice(base), rng)
                path = os.path.join(workdir, "case.bin")
                with open(path, "wb") as f:
                    f.write(data)

                cmd = ([kbinary, path] if parser == "ramki"
                       else [binary, parser, path])
                run = subprocess.run(cmd, capture_output=True, timeout=30)
                if run.returncode != 0:
                    crashes += 1
                    keep = os.path.join(ROOT, "fuzz-%s-%03d.bin" % (parser, i))
                    with open(keep, "wb") as f:
                        f.write(data)
                    failures.append((parser, keep,
                                     run.stderr.decode(errors="replace")))
                    if crashes >= 3:
                        break
            print("  %-6s %4d przypadkow, bledow: %d" %
                  (parser, CASES, crashes))
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    if failures:
        print("\nZNALEZIONE BLEDY:")
        for parser, path, err in failures[:5]:
            print("--- %s (%s) ---" % (parser, path))
            print("\n".join(err.strip().splitlines()[:12]))
        return 1

    print("\nWszystkie parsery przetrwaly uszkodzone dane.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
