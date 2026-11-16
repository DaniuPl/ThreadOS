#!/usr/bin/env python3
"""Checks the ThreadOS parsers against corrupted data.

Six fronts: the ring 3 programs (certificates, HTML, HTTP, cookies), the
packet path in the kernel (net.c + tcp.c), the kernel's TrueType
reader (ttf.c), because a font is a file and files come from anywhere,
the kernel's HID report descriptor reader (hid.c), because a descriptor
comes from whatever is plugged in, Mbed TLS's certificate and key
parsers, which curl hands whatever a server sends, and the ARM64
kernel's device tree reader (kernelarm64/fdt.c), because the tree is
the firmware's and the firmware is somebody else's. In the kernel an error does
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
import struct
import subprocess
import sys
import tempfile
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FUZZ = os.path.join(ROOT, "tests", "fuzz")
CASES = int(sys.argv[1]) if len(sys.argv) > 1 else 400


def build(workdir):
    """Compiles the harness. The userspace headers pull in 'uapi.h', the
    entry into the kernel, which makes no sense on the host. So we
    substitute a stub: a directory of symlinks to the real headers, and
    instead of the entry into the kernel a file with types and small
    details only."""
    inc = os.path.join(workdir, "include")
    os.makedirs(inc, exist_ok=True)

    stubbed = ("uapi.h",)
    for name in os.listdir(os.path.join(ROOT, "user")):
        if name.endswith(".h") and name not in stubbed:
            os.symlink(os.path.join(ROOT, "user", name),
                       os.path.join(inc, name))
    for name in stubbed:
        shutil.copy(os.path.join(FUZZ, "stub_usys.h"),
                    os.path.join(inc, name))
    # Brotli's fixed data, as the build makes it
    subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "mkbrotlidata.py"),
                    os.path.join(ROOT, "third_party", "brotli-rfc7932"),
                    os.path.join(inc, "ubrotlidata.h")], check=True)

    binary = os.path.join(workdir, "harness")
    cmd = ["gcc", "-I", inc, "-fsanitize=address,undefined",
           "-fno-omit-frame-pointer", "-g", "-O1", "-Wall",
           "-o", binary, os.path.join(FUZZ, "harness.c"), "-lm"]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr[:4000])
        raise SystemExit("the harness could not be built")
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
            raise SystemExit("%s could not be built" % src)
        objs.append(obj)

    binary = os.path.join(workdir, "kharness")
    cmd = ["gcc", "-I", inc, "-fsanitize=address,undefined", "-g", "-O1",
           "-Wall", "-o", binary,
           os.path.join(FUZZ, "kernel_harness.c")] + objs
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr[:3000])
        raise SystemExit("the kernel harness could not be built")
    return binary


def build_ttf(workdir):
    """The kernel's ttf.c, linked in kstub/ like net.c, with the same
    substituted headers."""
    kstub = os.path.join(FUZZ, "kstub")
    inc = os.path.join(kstub, "include")
    binary = os.path.join(workdir, "tharness")
    cmd = ["gcc", "-I", inc, "-fsanitize=address,undefined",
           "-fno-omit-frame-pointer", "-g", "-O1", "-Wall",
           "-o", binary, os.path.join(FUZZ, "ttf_harness.c"),
           os.path.join(kstub, "ttf.c")]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr[:3000])
        raise SystemExit("the TrueType harness could not be built")
    return binary


def build_hid(workdir):
    """The kernel's hid.c, linked in kstub/ like ttf.c"""
    kstub = os.path.join(FUZZ, "kstub")
    binary = os.path.join(workdir, "hharness")
    cmd = ["gcc", "-I", os.path.join(kstub, "include"),
           "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
           "-fno-omit-frame-pointer", "-g", "-O1", "-Wall",
           "-o", binary, os.path.join(FUZZ, "hid_harness.c"),
           os.path.join(kstub, "hid.c")]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr[:3000])
        raise SystemExit("the HID harness could not be built")
    return binary


def hid_seeds():
    """Report descriptors the way devices write them - a boot keyboard,
    media keys as an array under report ids, media keys as one bit
    each - with reports for them (scripts/usbredir.py builds the same
    ones for t_usbmedia)"""
    import struct
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    import usbredir as u
    media = (u.usage_page(0x0C) + u.usage(0x01) + u.collection(1) +
             u.report_id(2) + u.logical_min(0) + u.logical_max(0x23C) +
             u.usage_min(0) + u.usage_max(0x23C) + u.report_size(16) +
             u.report_count(1) + u.input_(0) + u.end_collection())
    bits = (u.usage_page(0x0C) + u.usage(0x01) + u.collection(1) +
            u.logical_min(0) + u.logical_max(1) + u.report_size(1) +
            u.report_count(8) + b"".join(u.usage(x) for x in
                                         (0xE9, 0xEA, 0xE2, 0xCD, 0xB5,
                                          0xB6, 0xB7, 0xB0)) +
            u.input_(2) + u.end_collection())

    def case(desc, *reports):
        return (struct.pack("<H", len(desc)) + desc +
                b"".join(bytes([len(r)]) + bytes(r) for r in reports))
    return [case(u.BOOT_KEYBOARD, [0, 0, 4, 5, 0, 0, 0, 0], [2] + [0] * 7),
            case(media, [2, 0xE9, 0], [2, 0, 0], [1, 1], [3, 1, 2]),
            case(bits, [1], [0xFF], [0]),
            case(u.BOOT_KEYBOARD + media, [2, 0xE2, 0], [0] * 8)]


MBED_FUZZERS = ("x509crt", "pubkey", "privkey")


def build_mbedtls(workdir):
    """Mbed TLS (third_party/, the archive the Makefile names) under the
    same sanitizers, with its own fuzz programs from programs/fuzz and
    the driver there that runs them on one file (onefile.c). Undefined
    behaviour ends the program here, so that it counts."""
    with open(os.path.join(ROOT, "Makefile")) as f:
        tar = next(l.split(":=")[1].strip() for l in f
                   if l.startswith("MBEDTLS_TAR"))
    src = os.path.join(workdir, "mbedtls")
    os.makedirs(src)
    subprocess.run(["tar", "-xf", os.path.join(ROOT, tar), "-C", src,
                    "--strip-components=1"], check=True)
    san = ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
           "-fno-omit-frame-pointer", "-g", "-O1"]
    out = subprocess.run(["make", "-s", "-j8", "-C",
                          os.path.join(src, "library"), "CC=gcc",
                          "CFLAGS=" + " ".join(san), "static"],
                         capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr[-3000:])
        raise SystemExit("Mbed TLS could not be built for fuzzing")
    lib = os.path.join(src, "library")
    fz = os.path.join(src, "programs", "fuzz")
    binaries = {}
    for name in MBED_FUZZERS:
        binary = os.path.join(workdir, "mbed-" + name)
        out = subprocess.run(
            ["gcc", *san, "-w", "-I", os.path.join(src, "include"),
             "-I", fz, "-o", binary, os.path.join(fz, "fuzz_%s.c" % name),
             os.path.join(fz, "common.c"), os.path.join(fz, "onefile.c"),
             os.path.join(lib, "libmbedx509.a"),
             os.path.join(lib, "libmbedcrypto.a")],
            capture_output=True, text=True)
        if out.returncode != 0:
            print(out.stderr[:3000])
            raise SystemExit("Mbed TLS's fuzz_%s could not be built" % name)
        binaries["mbedtls-" + name] = binary
    return binaries


def mbedtls_seeds(workdir):
    """Keys for Mbed TLS's key parsers, private and public, in DER and
    in PEM - the certificates come from seeds()"""
    pub, priv = [], []
    for tag in ("rsa", "p256", "p384"):
        key = os.path.join(workdir, tag + ".key")
        for form in ("DER", "PEM"):
            for args, bucket in ((["-in", key], priv),
                                 (["-in", key, "-pubout"], pub)):
                out = subprocess.run(["openssl", "pkey", *args, "-outform",
                                      form], capture_output=True, timeout=60)
                if out.returncode == 0:
                    bucket.append(out.stdout)
    return pub, priv
def build_fdt(workdir):
    """The ARM64 kernel's fdt.c, straight out of kernelarm64/, with the
    shared string.c it uses. Nothing to stub: it reads memory and
    nothing else."""
    binary = os.path.join(workdir, "fharness")
    cmd = ["gcc", "-I", os.path.join(ROOT, "kernelarm64"), "-fno-builtin",
           "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
           "-fno-omit-frame-pointer", "-g", "-O1", "-Wall", "-Wextra",
           "-o", binary, os.path.join(FUZZ, "fdt_harness.c"),
           os.path.join(ROOT, "kernelarm64", "fdt.c"),
           os.path.join(ROOT, "kernel", "string.c")]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr[:3000])
        raise SystemExit("the device tree harness could not be built")
    return binary


def make_fdt(mem_mib, cores, size_cells=2, more_memory=None, gic=2,
             bootargs=None, psci="hvc"):
    """A flattened device tree laid out the way QEMU's virt machine lays
    out its own: a UART, an interrupt controller (with a child node of
    its own), a memory node, the cores under /cpus next to a cpu-map
    that is not a core, and /chosen. Written from the specification
    (v17), not by the code under test."""
    strings, names = bytearray(), {}
    st = bytearray()

    def name_at(n):
        if n not in names:
            names[n] = len(strings)
            strings.extend(n.encode() + b"\0")
        return names[n]

    def pad(b):
        return b + b"\0" * (-len(b) % 4)

    def begin(n):
        st.extend(struct.pack(">I", 1) + pad(n.encode() + b"\0"))

    def end():
        st.extend(struct.pack(">I", 2))

    def prop(n, v):
        st.extend(struct.pack(">III", 3, len(v), name_at(n)) + pad(v))

    def cells(*v):
        return b"".join(struct.pack(">I", x) for x in v)

    def reg(base, size):
        if size_cells == 2:
            return cells(base >> 32, base & 0xFFFFFFFF, size >> 32,
                         size & 0xFFFFFFFF)
        return cells(base >> 32, base & 0xFFFFFFFF, size)

    def memory(base, size):
        begin("memory@%x" % base)
        prop("reg", reg(base, size))
        prop("device_type", b"memory\0")
        end()

    begin("")
    prop("#address-cells", cells(2))
    prop("#size-cells", cells(size_cells))
    prop("compatible", b"linux,dummy-virt\0")
    begin("pl011@9000000")
    prop("reg", reg(0x09000000, 0x1000))
    prop("compatible", b"arm,pl011\0arm,primecell\0")
    end()
    begin("intc@8000000")
    if gic == 2:
        # The name it is known by is not the first in the list
        prop("compatible", b"arm,arm-v8-gic\0arm,cortex-a15-gic\0")
        prop("reg", reg(0x08000000, 0x10000) + reg(0x08010000, 0x10000))
        begin("v2m@8020000")
        prop("compatible", b"arm,gic-v2m-frame\0")
        prop("reg", reg(0x08020000, 0x1000))
        end()
    else:
        prop("compatible", b"arm,gic-v3\0")
        prop("reg", reg(0x08000000, 0x10000) + reg(0x080A0000, 0xF60000))
    end()
    memory(0x40000000, mem_mib << 20)
    if more_memory:
        memory(*more_memory)
    begin("cpus")
    prop("#address-cells", cells(1))
    prop("#size-cells", cells(0))
    begin("cpu-map")
    begin("socket0")
    end()
    end()
    for i in range(cores):
        # virt's MPIDRs: eight cores to a cluster, the cluster in Aff1
        begin("cpu@%d" % i)
        prop("device_type", b"cpu\0")
        prop("reg", cells((i // 8) << 8 | i % 8))
        end()
    end()
    if psci:
        begin("psci")
        prop("compatible", b"arm,psci-1.0\0arm,psci-0.2\0arm,psci\0")
        prop("method", psci.encode() + b"\0")
        end()
    begin("chosen")
    if bootargs is not None:
        prop("bootargs", bootargs.encode() + b"\0")
    prop("stdout-path", b"/pl011@9000000\0")
    end()
    end()
    st.extend(struct.pack(">I", 9))

    reserve = b"\0" * 16
    off_struct = 40 + len(reserve)
    off_strings = off_struct + len(st)
    total = off_strings + len(strings)
    header = struct.pack(">10I", 0xD00DFEED, total, off_struct, off_strings,
                         40, 17, 16, 0, len(strings), len(st))
    return header + reserve + bytes(st) + bytes(strings)


_GIC2 = " gic=v2 dist=0x8000000 cpu=0x8010000"
FDT_TREES = [
    ("256 MiB and two cores", make_fdt(256, 2),
     "r=0 mem=256 MiB base=0x40000000 cpus=2" + _GIC2 +
     " args=- psci=hvc ids=0,1"),
    ("a gigabyte, four cores, smc and a command line",
     make_fdt(1024, 4, bootargs="selftest=brk quiet", psci="smc"),
     "r=0 mem=1024 MiB base=0x40000000 cpus=4" + _GIC2 +
     " args=selftest=brk quiet psci=smc ids=0,1,2,3"),
    # More cores than are kept: all counted, the first eight's MPIDRs
    # kept, and nothing written past them
    ("nine cores", make_fdt(256, 9),
     "r=0 mem=256 MiB base=0x40000000 cpus=9" + _GIC2 +
     " args=- psci=hvc ids=0,1,2,3,4,5,6,7"),
    # One-cell sizes, and memory in two places, the second above 4 GiB
    ("two memory ranges, one-cell sizes, no PSCI",
     make_fdt(512, 1, size_cells=1, more_memory=(0x100000000, 2048 << 20),
              psci=None),
     "r=0 mem=2560 MiB base=0x40000000 cpus=1" + _GIC2 +
     " args=- psci=- ids=0"),
    # The redistributors are its second range: not a CPU interface
    ("a GICv3", make_fdt(256, 1, gic=3),
     "r=0 mem=256 MiB base=0x40000000 cpus=1 gic=v3 dist=0x8000000 "
     "cpu=0x0 args=- psci=hvc ids=0"),
    ("a tree cut short", make_fdt(256, 2)[:-40],
     "r=-1 mem=0 MiB base=0x0 cpus=0 gic=v0 dist=0x0 cpu=0x0 args=- "
     "psci=- ids="),
]


def fdt_layouts(binary, workdir):
    """The undamaged trees, held to what they were made with. Returns
    (name, got, want) for every one that differs."""
    wrong = []
    path = os.path.join(workdir, "tree.dtb")
    for name, tree, want in FDT_TREES:
        with open(path, "wb") as f:
            f.write(tree)
        run = subprocess.run([binary, path], capture_output=True, text=True,
                             timeout=30)
        got = run.stdout.strip()
        if run.returncode != 0 or got != want:
            wrong.append((name, got or run.stderr[:300], want))
    return wrong


def fonts():
    """The two fonts the system carries - as valid a start as any."""
    out = []
    for name in ("sans.ttf", "mono.ttf"):
        with open(os.path.join(ROOT, "fonts", name), "rb") as f:
            out.append(f.read())
    return out


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

    icmp = b"\x08\x00\x00\x00\x12\x34\x00\x01" + b"thread" * 4
    out.append(ip(1, icmp))

    udp_payload = b"a test query"
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
           "inflate": [], "jpeg": [], "cookie": [], "gif": [], "svg": [], "webp": [],
           "brotli": [], "woff": []}

    for tag, keyargs in [("rsa", ["-newkey", "rsa:2048"]),
                         ("p256", ["-newkey", "ec", "-pkeyopt",
                                   "ec_paramgen_curve:prime256v1"]),
                         ("p384", ["-newkey", "ec", "-pkeyopt",
                                   "ec_paramgen_curve:secp384r1", "-sha384"])]:
        openssl("req", "-x509", *keyargs, "-nodes", "-keyout", j(tag + ".key"),
                "-out", j(tag + ".pem"), "-days", "3650",
                "-subj", "/CN=threados.test",
                "-addext", "subjectAltName=DNS:threados.test,DNS:*.test")
        openssl("x509", "-in", j(tag + ".pem"), "-outform", "DER",
                "-out", j(tag + ".der"))
        out["x509"].append(open(j(tag + ".der"), "rb").read())

    with open(j("msg"), "wb") as f:
        f.write(b"ThreadOS")
    openssl("dgst", "-sha256", "-sign", j("p256.key"), "-out", j("p256.sig"),
            j("msg"))
    out["ecdsa"].append(open(j("p256.sig"), "rb").read())

    out["html"].append(
        b"<html><head><title>x</title><style>a{}</style></head><body>"
        b"<h1>A heading</h1><p>Text with the entity &amp; and a link "
        b'<a href="/onwards">tutaj</a>.</p><ul><li>raz</li><li>dwa</li></ul>'
        b"<script>var x=1;</script></body></html>")
    out["html"].append(b"<p>" + b"a word " * 500 + b"</p>")

    out["http"].append(
        b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
        b"Transfer-Encoding: chunked\r\nLocation: /somewhere\r\n\r\n")

    # The picture decoders. A deflate stream at three levels, because
    # level zero writes stored blocks and the other two write both
    # kinds of tree; and the test harness's own JPEG, which came from
    # an outside encoder.
    body = b"ThreadOS: a picture that can be compressed. " * 40
    for level in (0, 6, 9):
        out["inflate"].append(zlib.compress(body, level))
    out["inflate"].append(zlib.compress(bytes(range(256)) * 8, 9))
    raw = zlib.compressobj(9, zlib.DEFLATED, -15)   # no zlib wrapper
    out["inflate"].append(raw.compress(body) + raw.flush())

    out["gif"] += [g for g, _ in gif_samples()]

    # The pictures the SVG test draws: every part of SVG it knows
    sys.path.insert(0, os.path.join(ROOT, "scripts", "tests"))
    from svgcases import CASES             # noqa: E402
    out["svg"] += [svg.encode() for _, _, _, svg in CASES]

    # WebP pictures libwebp wrote: lossy, lossless, alpha, an animation -
    # the small ones, so a case runs quickly
    import webpcases                       # noqa: E402
    if webpcases.available():
        out["webp"] += [data for _, data, _ in webpcases.cases()
                        if len(data) < 12000]

    # Brotli streams libbrotli wrote - text, a font, at several qualities
    # and windows - and fonts as WOFF and WOFF2 that fontTools wrote
    import brotliref                       # noqa: E402
    font = open(os.path.join(ROOT, "fonts", "mono.ttf"), "rb").read()
    if brotliref.install():
        text = open(os.path.join(ROOT, "README.md"), "rb").read()[:20000]
        for data, q, w in [(text, 11, 22), (text, 5, 16), (text, 1, 10), (font[:30000], 11, 18)]:
            out["brotli"].append(brotliref.compress(data, quality=q, lgwin=w))
    try:
        import io
        from fontTools.ttLib import TTFont, woff2
        f = TTFont(io.BytesIO(font))
        f.flavor = "woff"
        b = io.BytesIO()
        f.save(b)
        out["woff"].append(b.getvalue())
        if brotliref.available():
            for tables in ({"glyf", "loca"}, {"glyf", "loca", "hmtx"}):
                b = io.BytesIO()
                woff2.compress(io.BytesIO(font), b, transform_tables=tables)
                out["woff"].append(b.getvalue())
    except ImportError:
        pass
    for k in ("brotli", "woff"):
        if not out[k]:
            del out[k]

    # Set-Cookie headers as sites send them, a line each
    out["cookie"].append(
        b"session=abc123; Path=/; HttpOnly\n"
        b"id=7; Domain=.example.com; Max-Age=3600; Secure\n"
        b"theme=dark; Path=/a; Expires=Wed, 21 Oct 2037 07:28:00 GMT\n"
        b"old=1; Expires=Thu, 01 Jan 1970 00:00:00 GMT\n"
        b"evil=1; Domain=other.org\n"
        b"session=; Path=/; Max-Age=0\n")
    out["cookie"].append(b"a=" + b"x" * 500 + b"; Path=/" + b"p" * 200)

    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    from ptest import JPEG_IMAGE           # noqa: E402 - it lives there
    out["jpeg"].append(JPEG_IMAGE)
    return out


def gif_samples():
    """GIFs written by Pillow, with what Pillow reads back from each:
    plain and interlaced, a palette of 256 and of 4, one with a
    transparent colour - as (file, (w, h, rgba bytes))"""
    from PIL import Image
    import io
    out = []
    pics = []
    grad = Image.new("RGB", (37, 23))
    grad.putdata([(x * 7 % 256, y * 11 % 256, (x * y) % 256)
                  for y in range(23) for x in range(37)])
    pics.append((grad.quantize(256), {"interlace": False}))
    pics.append((grad.quantize(256), {"interlace": True}))
    small = Image.new("P", (16, 9))
    small.putpalette([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 0])
    small.putdata([(x // 4 + y // 3) % 4 for y in range(9) for x in range(16)])
    pics.append((small, {"transparency": 2, "interlace": False}))
    for pic, kw in pics:
        f = io.BytesIO()
        pic.save(f, "GIF", **kw)
        data = f.getvalue()
        back = Image.open(io.BytesIO(data)).convert("RGBA")
        if "transparency" in kw:
            # (what is transparent may hold any colour; only alpha says)
            back.putdata([p if p[3] else (0, 0, 0, 0) for p in back.getdata()])
        out.append((data, (back.width, back.height, back.tobytes())))
    return out


def gif_pixels(binary):
    """Our GIF decoder against Pillow's, picture by picture: the files that
    came out different"""
    wrong = []
    for data, (w, h, rgba) in gif_samples():
        path = os.path.join(os.path.dirname(binary), "sample.gif")
        with open(path, "wb") as f:
            f.write(data)
        run = subprocess.run([binary, "gifpixels", path], capture_output=True,
                             text=True, timeout=30)
        lines = run.stdout.split()
        if len(lines) < 3 or lines[:2] != [str(w), str(h)] or \
                bytes.fromhex(lines[2]) != rgba:
            wrong.append("%dx%d: %s" % (w, h, " ".join(lines[:2])))
    return wrong


def cookie_dates(binary, rng):
    """The cookie date reader against Python's calendar: dates as servers
    write them - RFC 1123, the old RFC 850 with two-digit years, and C's
    asctime - each must come out as the seconds timegm gives. Returns
    the dates it got wrong."""
    import calendar
    days = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"]
    long_days = ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
                 "Saturday", "Sunday"]
    months = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug",
              "Sep", "Oct", "Nov", "Dec"]
    lines, want = [], []
    for _ in range(300):
        t = time.gmtime(rng.randrange(0, 3155760000))      # 1970 to 2069
        sec = calendar.timegm(t)
        wd, mon = t.tm_wday, months[t.tm_mon - 1]
        hms = "%02d:%02d:%02d" % (t.tm_hour, t.tm_min, t.tm_sec)
        lines += ["%s, %02d %s %d %s GMT" % (days[wd], t.tm_mday, mon,
                                             t.tm_year, hms),
                  "%s, %02d-%s-%02d %s GMT" % (long_days[wd], t.tm_mday, mon,
                                               t.tm_year % 100, hms),
                  "%s %s %2d %s %d" % (days[wd], mon, t.tm_mday, hms,
                                       t.tm_year)]
        want += [sec] * 3
    path = os.path.join(os.path.dirname(binary), "dates.txt")
    with open(path, "w") as f:
        f.write("\n".join(lines))
    run = subprocess.run([binary, "cookiedate", path], capture_output=True,
                         text=True, timeout=30)
    got = [int(x) for x in run.stdout.split()]
    return [(d, g, w) for d, g, w in zip(lines, got, want) if g != w] + \
        ([("(count)", len(got), len(want))] if len(got) != len(want) else [])


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
    workdir = tempfile.mkdtemp(prefix="threados-fuzz-")
    failures = []

    try:
        binary = build(workdir)
        kbinary = build_kernel(workdir)
        tbinary = build_ttf(workdir)
        mbinaries = build_mbedtls(workdir)
        hbinary = build_hid(workdir)
        fbinary = build_fdt(workdir)
        corpus = seeds(workdir)
        corpus["frames"] = frames()
        corpus["ttf"] = fonts()
        corpus["hid"] = hid_seeds()
        corpus["fdt"] = [t for _, t, want in FDT_TREES
                         if want.startswith("r=0")]
        corpus["mbedtls-x509crt"] = list(corpus["x509"])
        corpus["mbedtls-pubkey"], corpus["mbedtls-privkey"] = \
            mbedtls_seeds(workdir)
        print("the harness is built, cases per parser: %d" % CASES)
        wrong = fdt_layouts(fbinary, workdir)
        print("  device trees against what they were made with: %d wrong"
              % len(wrong))
        for name, got, want in wrong:
            failures.append(("fdtlayout", name,
                             "got  %s\nwant %s" % (got, want)))
        bad = gif_pixels(binary)
        print("  GIFs against Pillow's reading of them: %d wrong" % len(bad))
        for b in bad:
            failures.append(("gifpixels", b, "the pixels differ"))
        wrong = fdt_layouts(fbinary, workdir)
        print("  device trees against what they were made with: %d wrong"
              % len(wrong))
        for name, got, want in wrong:
            failures.append(("fdtlayout", name,
                             "got  %s\nwant %s" % (got, want)))
        wrong = cookie_dates(binary, rng)
        print("  cookie dates against Python's calendar: %d wrong"
              % len(wrong))
        for d, g, w in wrong[:5]:
            failures.append(("cookiedate", d, "got %s, want %s" % (g, w)))

        for parser, base in corpus.items():
            crashes = 0
            for i in range(CASES):
                data = mutate(rng.choice(base), rng)
                path = os.path.join(workdir, "case.bin")
                with open(path, "wb") as f:
                    f.write(data)

                if parser == "frames":
                    cmd = [kbinary, path]
                elif parser == "ttf":
                    cmd = [tbinary, path]
                elif parser == "hid":
                    cmd = [hbinary, path]
                elif parser in mbinaries:
                    cmd = [mbinaries[parser], path]
                elif parser == "fdt":
                    cmd = [fbinary, path]
                else:
                    cmd = [binary, parser, path]
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
            print("  %-15s %4d cases, errors: %d" %
                  (parser, CASES, crashes))
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    if failures:
        print("\nERRORS FOUND:")
        for parser, path, err in failures[:5]:
            print("--- %s (%s) ---" % (parser, path))
            print("\n".join(err.strip().splitlines()[:12]))
        return 1

    print("\nEvery parser survived the corrupted data.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
