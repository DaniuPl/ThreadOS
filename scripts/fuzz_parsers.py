#!/usr/bin/env python3
"""Checks the ThreadOS parsers against corrupted data.

Six fronts: the ring 3 programs (certificates, HTML, HTTP, cookies), the
packet path in the kernel (net.c + tcp.c), the kernel's TrueType
reader (ttf.c), because a font is a file and files come from anywhere,
the kernel's HID report descriptor reader (hid.c), because a descriptor
comes from whatever is plugged in, the kernel's WPA2 handshake (wpa.c)
and 802.11 frame reader (wlan.c), because their frames come from the air,
TFS4 (tfs4.c, the ext2 format), because a disk is somebody else's too, Mbed TLS's certificate and key
parsers, which curl hands whatever a server sends, and the ARM64
kernel's device tree reader (kernelarm64/fdt.c), because the tree is
the firmware's and the firmware is somebody else's - and GLib, under
WebKit's processes, with its own fuzzers: key files, GVariant in text
and binary, URIs, dates, bookmark files (its XML), UTF-8, D-Bus
messages, DNS replies and network addresses; and what a web page's
pictures, documents and fonts go through: libpng, libjpeg-turbo and
libxml2 with their own fuzzers, FreeType with ours, and what WPE's
WebKit adds - libwebp and HarfBuzz with ours, libtasn1's DER with its
own; and what the web engine's network process reads from servers:
OpenSSL's certificates, ASN.1, PEM and both ends of a TLS handshake
with its own fuzzers,
HTTP/2 frames through nghttp2 with ours, the public suffix list
(libpsl) and libsoup's HTTP headers, cookies, dates, data: URIs and
content sniffing with theirs. In the kernel an error does
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


def build_wpa(workdir):
    """The kernel's wpa.c, with the programs' WPA and AES headers it
    builds on (user/uwpa.h, user/uaes.h)"""
    binary = os.path.join(workdir, "wharness")
    must(["gcc", *SAN, "-w", "-I", ROOT, "-o", binary,
          os.path.join(FUZZ, "wpa_harness.c"),
          os.path.join(ROOT, "kernel", "wpa.c")], "the WPA harness")
    return binary


def build_wlan(workdir):
    """The kernel's wlan.c: the 802.11 frames a station reads"""
    binary = os.path.join(workdir, "lharness")
    must(["gcc", *SAN, "-w", "-I", ROOT, "-o", binary,
          os.path.join(FUZZ, "wlan_harness.c"),
          os.path.join(ROOT, "kernel", "wlan.c")], "the 802.11 harness")
    return binary


def build_tfs4(workdir):
    """The kernel's tfs4.c, with stand-ins for what it reaches past the
    disk: interrupts, the heap (the host's), the partition table and
    the task's rights. The block layer is the harness's own."""
    tree = os.path.join(workdir, "tfs4")
    inc = os.path.join(tree, "kernel", "include")
    os.makedirs(inc)
    os.makedirs(os.path.join(tree, "kernel64"))
    for name in ("tfs4.h", "fsbackend.h", "fs.h", "part.h", "rtc.h"):
        shutil.copy(os.path.join(ROOT, "kernel", "include", name), inc)
    shutil.copy(os.path.join(ROOT, "kernel", "tfs4.c"),
                os.path.join(tree, "kernel"))
    stubs = {
        "blk.h": "#include <stdint.h>\nint blk_present(void);\n"
                 "uint32_t blk_sectors(void);\n"
                 "int blk_read(uint32_t lba, uint32_t n, void *buf);\n"
                 "int blk_write(uint32_t lba, uint32_t n, const void *buf);\n",
        "random.h": "#include <stdint.h>\n"
                    "void random_bytes(void *out, uint32_t n);\n",
        "heap.h": "#include <stdlib.h>\n#define kmalloc malloc\n"
                  "#define kfree free\n",
        "string.h": "#include <string.h>\n",
        "printk.h": "int printk(const char *fmt, ...);\n",
        "io.h": "typedef unsigned long irqflags_t;\n"
                "static inline irqflags_t irq_save(void) { return 0; }\n"
                "static inline void irq_restore(irqflags_t f) { (void)f; }\n",
        "user.h": "#define USER_ADMIN 2\n"
                  "int user_is_system_file(const char *name);\n",
    }
    for name, text in stubs.items():
        with open(os.path.join(inc, name), "w") as f:
            f.write(text)
    with open(os.path.join(tree, "kernel64", "task64.h"), "w") as f:
        f.write("int task64_level(void);\nint task64_uid(void);\n"
                "int task64_slot_open(int slot);\n"
                "int task64_umask(void);\n")
    binary = os.path.join(workdir, "t4harness")
    must(["gcc", *SAN, "-w", "-I", os.path.join(tree, "kernel"), "-o", binary,
          os.path.join(FUZZ, "tfs4_harness.c"),
          os.path.join(tree, "kernel", "tfs4.c")], "the TFS4 harness")
    return binary


def tfs4_seeds(workdir):
    """Small ext2 images made by Linux's mke2fs: 1 KiB and 4 KiB blocks,
    a file past the direct blocks, a directory, symbolic links short and
    long, a file with a hole"""
    if not shutil.which("mke2fs"):
        return []
    tree = os.path.join(workdir, "tfs4-tree")
    os.makedirs(os.path.join(tree, "dir", "sub"))
    with open(os.path.join(tree, "dir", "sub", "f.txt"), "wb") as f:
        f.write(b"a file two directories down\n")
    with open(os.path.join(tree, "big"), "wb") as f:
        f.write(bytes(range(256)) * 80)
    with open(os.path.join(tree, "holey"), "wb") as f:
        f.write(b"x")
        f.seek(40000)
        f.write(b"y")
    os.symlink("dir/sub/f.txt", os.path.join(tree, "link"))
    os.symlink("./dir/./sub/./f.txt/../../../dir/sub/f.txt" + "/." * 20,
               os.path.join(tree, "longlink"))
    seeds = []
    for bs, size in ((1024, "160K"), (4096, "512K")):
        img = os.path.join(workdir, "seed-%d.img" % bs)
        subprocess.run(["truncate", "-s", size, img], check=True)
        subprocess.run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", str(bs),
                        "-N", "32", "-L", "ThreadOS", "-d", tree, img],
                       check=True)
        with open(img, "rb") as f:
            seeds.append(f.read())
    return seeds


def tfs4_e2fsck(binary, workdir, seeds):
    """What TFS4 writes, judged by Linux's e2fsck: the kernel's mkfs on
    disks of one to nine block groups, and mke2fs's own images after the
    harness's writing (a file made, filled past its direct blocks, cut,
    moved, removed; a file renamed and removed). A clean disk in has to
    come out clean - e2fsck -fn says nothing it would fix."""
    wrong = []
    if not shutil.which("e2fsck"):
        return [("e2fsck", "e2fsprogs is not installed")]
    out = os.path.join(workdir, "tfs4-out.img")
    cases = [(["--format", str(m)], "mkfs of %d MiB" % m)
             for m in (2, 64, 130, 300, 1100)]
    for i, seed in enumerate(seeds):
        p = os.path.join(workdir, "tfs4-seed%d.img" % i)
        with open(p, "wb") as f:
            f.write(seed)
        cases.append(([p], "writing on mke2fs's image %d" % i))
    for args, what in cases:
        run = subprocess.run([binary] + args, capture_output=True,
                             env={"TFS4_OUT": out}, timeout=120)
        fsck = subprocess.run(["e2fsck", "-fn", out], capture_output=True,
                              text=True)
        if run.returncode or fsck.returncode or "? no" in fsck.stdout:
            wrong.append((what, fsck.stdout[-800:] + run.stderr.decode(
                errors="replace")[-800:]))
        if os.path.exists(out):
            os.unlink(out)
    return wrong


def wlan_seeds():
    """A beacon with the elements a station reads, an authentication and
    an association response, a deauthentication, data frames from the DS
    plain, QoS and protected"""
    me = bytes([0x7C, 0x5C, 0xF8, 0x12, 0x34, 0x56])
    ap = bytes([0x02, 0x7A, 0, 0, 0, 1])

    def hdr(fc, fl, a1, a2, a3):
        return bytes([fc, fl, 0, 0]) + a1 + a2 + a3 + bytes(2)
    beacon = (hdr(0x80, 0, b"\xff" * 6, ap, ap) + bytes(8) +
              bytes([100, 0, 0x11, 0x04]) + bytes([0, 4]) + b"Home" +
              bytes([1, 8, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24,
                     50, 4, 0x30, 0x48, 0x60, 0x6C, 3, 1, 6, 5, 4, 1, 2, 0, 0,
                     48, 20, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 4,
                     1, 0, 0, 0x0F, 0xAC, 2, 0x0C, 0, 221, 7, 0, 0x50, 0xF2,
                     2, 0, 1, 0]))
    auth = hdr(0xB0, 0, me, ap, ap) + bytes([0, 0, 2, 0, 0, 0])
    assoc = hdr(0x10, 0, me, ap, ap) + bytes([0x11, 4, 0, 0, 1, 0xC0,
                                               1, 4, 0x82, 0x84, 0x8B, 0x96])
    deauth = hdr(0xC0, 0, me, ap, ap) + bytes([3, 0])
    snap = bytes([0xAA, 0xAA, 3, 0, 0, 0, 0x08, 0]) + bytes(range(40))
    data = hdr(0x08, 0x02, me, ap, ap) + snap
    qos = hdr(0x88, 0x02, me, ap, ap) + bytes(2) + snap
    prot = (hdr(0x08, 0x42, me, ap, ap) + bytes([1, 0, 0, 0x20, 0, 0, 0, 0])
            + snap + bytes(8))
    return [beacon, auth, assoc, deauth, data, qos, prot]


def wpa_seeds():
    """What message 3 and the group handshake carry - the access point's
    RSN element and the group key in its KDE, padded - after the byte
    that picks the key information; and a message 1 and a message 3 as
    whole frames"""
    rsn = bytes([0x30, 20, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 4,
                 1, 0, 0, 0x0F, 0xAC, 2, 0x0C, 0])
    gtk = bytes([0xDD, 22, 0, 0x0F, 0xAC, 1, 1, 0]) + bytes(range(16))
    m1 = (bytes([2, 3, 0, 95, 2, 0, 0x8A, 0, 16]) + bytes(7) + b"\x01" +
          b"\x99" * 32 + bytes(16 + 8 + 8 + 16) + bytes(2))
    m3 = (bytes([2, 3, 0, 95 + 56, 2, 0x13, 0xCA, 0, 16]) + bytes(7) +
          b"\x02" + b"\x99" * 32 + bytes(48) + bytes([0, 56]) +
          bytes(range(56)))
    return [b"\x00" + rsn + gtk + b"\xdd\x00",
            b"\x01" + gtk + rsn,
            b"\x02" + rsn,
            b"\x00" + bytes([0xDD, 6, 0, 0x0F, 0xAC, 1, 3, 0]) + rsn,
            m1, m3]


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


# GLib's own fuzzers (fuzzing/ in its archive) that read what can come
# from outside: files, URIs, messages, network replies
GLIB_FUZZERS = ("key", "variant_text", "variant_binary", "uri_parse",
                "uri_parse_params", "bookmark",
                "date_time_new_from_iso8601", "utf8_validate",
                "utf8_normalize", "data_input_stream_read_line",
                "dbus_message", "resolver", "inet_address_new_from_string",
                "network_address_parse_uri")


def build_glib(workdir):
    """GLib (third_party/, the archive the Makefile names) built for the
    host by its own meson under the sanitizers, with libffi, PCRE2 and
    zlib from the host, and its fuzzers linked against it with their
    driver for one file (driver.c)"""
    with open(os.path.join(ROOT, "Makefile")) as f:
        tar = next(l.split(":=")[1].strip() for l in f
                   if l.startswith("GLIB_TAR "))
    src = os.path.join(workdir, "glib")
    os.makedirs(src)
    subprocess.run(["tar", "-xf", os.path.join(ROOT, tar), "-C", src,
                    "--strip-components=1"], check=True)
    prefix = os.path.join(workdir, "glib-root")
    out = subprocess.run(
        ["meson", "setup", "build", "--wrap-mode=nodownload",
         "--prefix=" + prefix, "--libdir=lib", "--buildtype=debugoptimized",
         "-Ddefault_library=static", "-Db_sanitize=address,undefined",
         "-Dtests=false", "-Dintrospection=disabled", "-Dnls=disabled",
         "-Dlibmount=disabled", "-Dselinux=disabled", "-Dxattr=false",
         "-Dlibelf=disabled", "-Dsysprof=disabled", "-Ddocumentation=false",
         "-Dman-pages=disabled"], cwd=src, capture_output=True, text=True)
    if out.returncode == 0:
        out = subprocess.run(["ninja", "-C", "build", "install"], cwd=src,
                             capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stdout[-3000:], out.stderr[-2000:])
        raise SystemExit("GLib could not be built for fuzzing")
    env = dict(os.environ,
               PKG_CONFIG_PATH=os.path.join(prefix, "lib", "pkgconfig"))
    flags = subprocess.run(["pkg-config", "--static", "--cflags", "--libs",
                            "gio-2.0"], env=env, capture_output=True,
                           text=True, check=True).stdout.split()
    san = ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
           "-fno-omit-frame-pointer", "-g", "-O1"]
    fz = os.path.join(src, "fuzzing")
    binaries = {}
    for name in GLIB_FUZZERS:
        binary = os.path.join(workdir, "glib-" + name)
        out = subprocess.run(
            # (the fuzzers name GLib's headers as its source tree has
            # them, "glib/glib.h")
            ["gcc", *san, "-w", "-I", fz, "-I", src, "-o", binary,
             os.path.join(fz, "driver.c"), os.path.join(fz, "fuzz_%s.c" % name),
             *flags], capture_output=True, text=True)
        if out.returncode != 0:
            print(out.stderr[:3000])
            raise SystemExit("GLib's fuzz_%s could not be built" % name)
        binaries["glib-" + name] = binary
    return binaries, fz


def glib_seeds(fz):
    """What each of GLib's fuzzers starts from: its own corpora where it
    has them, and a few well-formed inputs of each kind"""
    def corpus(name):
        d = os.path.join(fz, "fuzz_%s.corpus" % name)
        if not os.path.isdir(d):
            return []
        return [open(os.path.join(d, f), "rb").read()
                for f in sorted(os.listdir(d))]
    uris = [b"https://user:pw@example.org:8080/a/b%20c?x=1&y=%C5%BC#frag",
            b"file:///home/user/a%20file.txt", b"http://[::1]:80/",
            b"mailto:someone@example.org", b"data:text/plain;base64,QUJD"]
    variant = [b"{'name': <'ThreadOS'>, 'list': <[1, 2, 3]>}",
               b"(true, @y 7, [@u 1, 2], {'a': <@d 1.5>})",
               b"@a{sv} {'x': <'y'>}", b"[<(1, 'two')>, <@ay [0x01]>]"]
    return {
        "glib-key": corpus("key") or [
            b"[group]\nkey=value\nlist=a;b;c;\n[other]\nnum=12\n"],
        "glib-variant_text": variant,
        "glib-variant_binary": [b"\x00\x01\x02\x03ThreadOS\x00\x00s",
                                b"(\x00\x00\x00\x01\x02\x03\x04)",
                                b"\x01\x00\x00\x00\x02\x00\x00\x00au"],
        "glib-uri_parse": uris,
        "glib-uri_parse_params": [b"a=1&b=2;c=%C5%BC&d", b"x=&=y&&z=1"],
        "glib-bookmark": corpus("bookmark") or [
            b'<?xml version="1.0"?><xbel version="1.0"></xbel>'],
        "glib-date_time_new_from_iso8601": [
            b"2026-11-17T01:02:03Z", b"2026-W46-2T10:00:00+01:00",
            b"2026-321T23:59:60.123456-05:30"],
        "glib-utf8_validate": ["Zażółć gęślą jaźń".encode(),
                               b"\xf0\x9f\x98\x80 \xe2\x82\xac"],
        "glib-utf8_normalize": ["é Å ﬁ".encode(),
                                "Zażółć".encode()],
        "glib-data_input_stream_read_line": [b"one\ntwo\r\nthree\rfour"],
        "glib-dbus_message": [
            # a method call, little-endian, as GDBus writes one
            b"l\x01\x00\x01\x00\x00\x00\x00\x01\x00\x00\x00"
            b"m\x00\x00\x00\x01\x01o\x00\x15\x00\x00\x00"
            b"/org/freedesktop/DBus\x00\x00\x00\x02\x01s\x00"
            b"\x14\x00\x00\x00org.freedesktop.DBus\x00\x00\x00\x00"
            b"\x03\x01s\x00\x05\x00\x00\x00Hello\x00\x00\x00"
            b"\x06\x01s\x00\x14\x00\x00\x00org.freedesktop.DBus\x00"],
        "glib-resolver": [
            # a DNS reply: one question, one A record
            b"\x12\x34\x81\x80\x00\x01\x00\x01\x00\x00\x00\x00"
            b"\x07example\x03org\x00\x00\x01\x00\x01"
            b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x0e\x10\x00\x04"
            b"\x5d\xb8\xd7\x0e"],
        "glib-inet_address_new_from_string": [b"192.168.1.20", b"fe80::1",
                                              b"::ffff:10.0.2.15"],
        "glib-network_address_parse_uri": uris,
    }


# The libraries a web page's pictures, documents and fonts go through
# (Makefile, 'Libraries for WebKit's processes'): libpng's and
# libjpeg-turbo's own fuzzers, libxml2's, and ours for FreeType, whose
# archive has none
MEDIA_FUZZERS = {
    "libpng": "libpng_read_fuzzer",
    "libjpeg": "decompress",
    "libjpeg-api": "decompress_libjpeg",
    "libxml2-xml": "xml",
    "libxml2-reader": "reader",
    "libxml2-valid": "valid",
    "libxml2-xpath": "xpath",
    "libxml2-uri": "uri",
    "freetype": "freetype",
}
SAN = ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
       "-fno-omit-frame-pointer", "-g", "-O1"]


def tarball(var):
    with open(os.path.join(ROOT, "Makefile")) as f:
        return os.path.join(ROOT, next(l.split(":=")[1].strip() for l in f
                                       if l.startswith(var + " ")))


def unpack(var, workdir, name):
    src = os.path.join(workdir, name)
    os.makedirs(src)
    subprocess.run(["tar", "-xf", tarball(var), "-C", src,
                    "--strip-components=1"], check=True)
    return src


def must(cmd, what, **kw):
    out = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if out.returncode != 0:
        print(out.stdout[-2000:], out.stderr[-2000:])
        raise SystemExit(what + " could not be built for fuzzing")


def build_media(workdir):
    """libpng, libjpeg-turbo, libxml2 and FreeType built for the host
    under the sanitizers from the archives the Makefile names, each
    fuzzer linked with tests/fuzz/onefile.c"""
    cflags = "CFLAGS=" + " ".join(SAN)
    driver = os.path.join(ROOT, "tests", "fuzz", "onefile.c")
    binaries = {}

    png = unpack("LIBPNG_TAR", workdir, "libpng")
    must(["./configure", "--disable-shared", "--enable-static", cflags],
         "libpng", cwd=png)
    must(["make", "-j8", "libpng16.la"], "libpng", cwd=png)
    fz = os.path.join(png, "contrib", "oss-fuzz")
    b = os.path.join(workdir, "fz-libpng")
    # (nalloc.h, its allocation-failure wrapper, takes the sanitizer's
    # own allocator only when it sees clang; with gcc it is told)
    must(["g++", *SAN, "-DNALLOC_ASAN=1", "-w", "-I", png, "-I", fz, "-o", b,
          os.path.join(fz, "libpng_read_fuzzer.cc"), "-x", "c", driver,
          "-x", "none", os.path.join(png, ".libs", "libpng16.a"), "-lz"],
         "libpng's fuzzer")
    binaries["libpng"] = b

    jpeg = unpack("LIBJPEG_TAR", workdir, "libjpeg")
    must(["cmake", "-S", jpeg, "-B", os.path.join(jpeg, "b"), "-G", "Ninja",
          "-DENABLE_SHARED=OFF", "-DWITH_SIMD=OFF", "-DWITH_TOOLS=OFF",
          "-DWITH_TESTS=OFF", "-DCMAKE_C_FLAGS=" + " ".join(SAN)],
         "libjpeg-turbo")
    must(["ninja", "-C", os.path.join(jpeg, "b"), "turbojpeg-static",
          "jpeg-static"], "libjpeg-turbo")
    for name, src in (("libjpeg", "decompress"),
                      ("libjpeg-api", "decompress_libjpeg")):
        b = os.path.join(workdir, "fz-" + name)
        must(["g++", *SAN, "-w", "-I", jpeg, "-I", os.path.join(jpeg, "src"),
              "-I", os.path.join(jpeg, "b"), "-o", b,
              os.path.join(jpeg, "fuzz", src + ".cc"), "-x", "c", driver,
              "-x", "none", os.path.join(jpeg, "b", "libturbojpeg.a"),
              os.path.join(jpeg, "b", "libjpeg.a")], "libjpeg-turbo's " + src)
        binaries[name] = b

    xml = unpack("LIBXML2_TAR", workdir, "libxml2")
    must(["./configure", "--disable-shared", "--enable-static",
          "--without-python", "--without-readline", "--without-http",
          cflags], "libxml2", cwd=xml)
    must(["make", "-j8", "libxml2.la"], "libxml2", cwd=xml)
    for name in ("xml", "reader", "valid", "xpath", "uri"):
        b = os.path.join(workdir, "fz-libxml2-" + name)
        must(["gcc", *SAN, "-w", "-I", os.path.join(xml, "include"),
              "-I", xml, "-o", b, os.path.join(xml, "fuzz", name + ".c"),
              os.path.join(xml, "fuzz", "fuzz.c"), driver,
              os.path.join(xml, ".libs", "libxml2.a"), "-lz", "-lm"],
             "libxml2's fuzz " + name)
        binaries["libxml2-" + name] = b

    ft = unpack("FT_TAR", workdir, "freetype")
    must(["./configure", "--disable-shared", "--enable-static",
          "--with-zlib=yes", "--with-png=no", "--with-harfbuzz=no",
          "--with-brotli=no", "--with-bzip2=no", cflags], "FreeType", cwd=ft)
    must(["make", "-j8"], "FreeType", cwd=ft)
    b = os.path.join(workdir, "fz-freetype")
    must(["gcc", *SAN, "-w", "-I", os.path.join(ft, "include"), "-o", b,
          os.path.join(ROOT, "tests", "fuzz", "freetype_harness.c"), driver,
          os.path.join(ft, "objs", ".libs", "libfreetype.a"), "-lz"],
         "the FreeType harness")
    binaries["freetype"] = b
    return binaries


def build_opus(workdir):
    """libopus (third_party/opus) built for the host by meson under the
    sanitizers, and the harness (tests/fuzz/opus_harness.c) linked to
    it; and seeds made by that same libopus: a tone encoded, mono and
    stereo, at a few bit rates and frame lengths, as the harness reads
    them - packets after their lengths"""
    src = unpack("OPUS_TAR", workdir, "opus")
    prefix = os.path.join(workdir, "opus-root")
    must(["meson", "setup", "build", "--wrap-mode=nodownload",
          "--prefix=" + prefix, "--libdir=lib", "--buildtype=debugoptimized",
          "-Ddefault_library=static", "-Db_sanitize=address,undefined",
          "-Dextra-programs=disabled", "-Dtests=disabled", "-Ddocs=disabled"],
         "libopus", cwd=src)
    must(["ninja", "-C", "build", "install"], "libopus", cwd=src)
    inc = "-I" + os.path.join(prefix, "include", "opus")
    lib = os.path.join(prefix, "lib", "libopus.a")
    binary = os.path.join(workdir, "fz-opus")
    must(["gcc", *SAN, "-w", inc, "-o", binary,
          os.path.join(ROOT, "tests", "fuzz", "opus_harness.c"),
          os.path.join(ROOT, "tests", "fuzz", "onefile.c"), lib, "-lm"],
         "the Opus harness")
    maker = os.path.join(workdir, "opus-seeds.c")
    with open(maker, "w") as fh:
        fh.write(r"""
#include <math.h>
#include <stdio.h>
#include <opus.h>
int main(int argc, char **argv)
{
    int channels = argv[1][0] - '0', rate = atoi(argv[2]);
    int frame = atoi(argv[3]), err;
    OpusEncoder *e = opus_encoder_create(48000, channels,
                                         OPUS_APPLICATION_AUDIO, &err);
    opus_encoder_ctl(e, OPUS_SET_BITRATE(rate));
    static opus_int16 pcm[2880 * 2];
    unsigned char out[4000];
    putchar(channels - 1);
    for (int k = 0, t = 0; k < 6; k++) {
        for (int i = 0; i < frame; i++, t++)
            for (int c = 0; c < channels; c++)
                pcm[i * channels + c] =
                    (opus_int16)(8000 * sin(t * 2 * 3.14159265 * 1000 / 48000));
        int n = opus_encode(e, pcm, frame, out, sizeof out);
        putchar(n >> 8), putchar(n & 255);
        fwrite(out, 1, n, stdout);
    }
    opus_encoder_destroy(e);
    return 0;
}
""")
    must(["gcc", *SAN, "-include", "stdlib.h", inc, "-o", maker[:-2], maker,
          lib, "-lm"], "the Opus seed maker")
    seeds = []
    for args in (("1", "16000", "960"), ("2", "96000", "960"),
                 ("2", "32000", "480"), ("1", "64000", "2880")):
        out = subprocess.run([maker[:-2], *args], capture_output=True)
        if out.returncode == 0:
            seeds.append(out.stdout)
    return {"opus": binary}, {"opus": seeds}


def gst_tarball(module):
    """A GStreamer module's archive, as the Makefile names its version"""
    with open(os.path.join(ROOT, "Makefile")) as f:
        ver = next(l.split(":=")[1].strip() for l in f
                   if l.startswith("GST_VER "))
    return os.path.join(ROOT, "third_party", "gstreamer",
                        "%s-%s.tar.xz" % (module, ver))


def build_gst(workdir):
    """GStreamer's core, base and good (third_party/gstreamer) built for
    the host by meson under the sanitizers - static, with only the
    plugins a page's containers go through - and the harness
    (tests/fuzz/gst_demux_harness.c) linked once for each demuxer: MP4
    and WebM/Matroska"""
    prefix = os.path.join(workdir, "gst-root")
    env = dict(os.environ,
               PKG_CONFIG_PATH=os.path.join(prefix, "lib", "pkgconfig"))
    plain = ["-Dexamples=disabled", "-Dnls=disabled"]
    for module, opts in (
            ("gstreamer", plain + ["-Dintrospection=disabled",
                                   "-Dtools=disabled", "-Dbenchmarks=disabled",
                                   "-Dcheck=disabled",
                                   "-Dptp-helper=disabled"]),
            ("gst-plugins-base", plain + ["-Dintrospection=disabled",
                                          "-Dapp=enabled", "-Dgl=disabled",
                                          "-Dtools=disabled"]),
            ("gst-plugins-good", plain + ["-Disomp4=enabled",
                                          "-Dmatroska=enabled"])):
        src = os.path.join(workdir, module)
        os.makedirs(src)
        subprocess.run(["tar", "-xf", gst_tarball(module), "-C", src,
                        "--strip-components=1"], check=True)
        must(["meson", "setup", "build", "--wrap-mode=nodownload",
              "--prefix=" + prefix, "--libdir=lib",
              "--buildtype=debugoptimized", "-Ddefault_library=static",
              "-Db_sanitize=address,undefined", "-Dauto_features=disabled",
              "-Dtests=disabled", "-Ddoc=disabled", *opts],
             module, cwd=src, env=env)
        must(["ninja", "-C", "build", "install"], module, cwd=src, env=env)
    flags = subprocess.run(
        ["pkg-config", "--static", "--cflags", "--libs", "gstreamer-app-1.0",
         "gstreamer-riff-1.0", "gstreamer-tag-1.0", "gstreamer-audio-1.0",
         "gstreamer-video-1.0", "gstreamer-pbutils-1.0", "gstreamer-rtp-1.0",
         "zlib"], env=env, capture_output=True, text=True,
        check=True).stdout.split()
    plugins = os.path.join(prefix, "lib", "gstreamer-1.0")
    binaries = {}
    for name, demux, caps in (("gst-qtdemux", "qtdemux", "video/quicktime"),
                              ("gst-matroska", "matroskademux",
                               "video/x-matroska")):
        b = os.path.join(workdir, "fz-" + name)
        must(["gcc", *SAN, "-w", '-DDEMUX="%s"' % demux,
              '-DCAPS="%s"' % caps, "-o", b,
              os.path.join(ROOT, "tests", "fuzz", "gst_demux_harness.c"),
              os.path.join(ROOT, "tests", "fuzz", "onefile.c"),
              "-L" + plugins, "-lgstisomp4", "-lgstmatroska", "-lgstapp",
              "-lgstcoreelements", *flags], "the GStreamer harness")
        binaries[name] = b
    return binaries


def gst_seeds(workdir):
    """MP4 and WebM files to corrupt, made by the host's FFmpeg (none
    without one): H.264 and AAC in MP4, VP9 and Opus in WebM, short"""
    seeds = {"gst-qtdemux": [], "gst-matroska": []}
    if not shutil.which("ffmpeg"):
        return seeds
    for name, out, args in (
            ("gst-qtdemux", "seed.mp4", ["-c:v", "libx264", "-c:a", "aac"]),
            ("gst-matroska", "seed.webm", ["-c:v", "libvpx-vp9",
                                           "-c:a", "libopus"])):
        path = os.path.join(workdir, out)
        r = subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f",
             "lavfi", "-i", "testsrc2=size=64x48:rate=10:duration=0.5",
             "-f", "lavfi", "-i", "sine=duration=0.5", *args, path],
            capture_output=True)
        if r.returncode == 0:
            seeds[name].append(open(path, "rb").read())
    return seeds


def media_seeds(corpus):
    """Pictures from the decoders of our own above, the XML test's
    documents in the form libxml2's fuzzers read (options, then each
    entity's name and text ending in a backslash and a newline), and
    the system's fonts"""
    def entity(text):
        return text.replace(b"\\", b"\\\\") + b"\\\n"
    docs = []
    xdir = os.path.join(ROOT, "scripts", "tests", "xml")
    for name in sorted(os.listdir(xdir)):
        if name.endswith(".xml"):
            docs.append((name.encode(), open(os.path.join(xdir, name),
                                             "rb").read()))
    xml, reader, xpath, uri = [], [], [], []
    for name, text in docs:
        ents = entity(name) + entity(text)
        xml.append(struct.pack(">III", 0, 0, 0) + ents)
        reader.append(struct.pack(">II", 0, 0) + entity(b"") + ents)
        xpath.append(struct.pack(">I", 0) + entity(b"//*[@id] | count(//*)")
                     + entity(text))
    for u in (b"https://user@example.org:8080/a/b?c=d#e", b"../up/./x.svg",
              b"file:///home/a%20b.xml", b"urn:isbn:0451450523"):
        uri.append(struct.pack(">I", 0) + entity(u) +
                   entity(b"http://base.example/dir/page.html"))
    fonts = [open(os.path.join(ROOT, "fonts", f), "rb").read()
             for f in sorted(os.listdir(os.path.join(ROOT, "fonts")))
             if f.endswith(".ttf")]
    return {
        "libpng": list(corpus.get("png", [])),
        "libjpeg": list(corpus.get("jpeg", [])),
        "libjpeg-api": list(corpus.get("jpeg", [])),
        "libxml2-xml": xml, "libxml2-reader": reader,
        "libxml2-valid": [struct.pack(">II", 0, 0) + entity(n) + entity(t)
                          for n, t in docs],
        "libxml2-xpath": xpath, "libxml2-uri": uri,
        "freetype": fonts,
    }


# What WPE's port of WebKit adds that reads what a page sends (Makefile,
# 'What WPE's port of WebKit needs'): libwebp with ours - its own are
# written for FuzzTest - libtasn1's own two for DER, and ours for
# HarfBuzz, whose kept src/ has none

def build_wpe(workdir):
    """libwebp (its SIMD as ThreadOS has it: SSE2, no SSE4.1 or AVX2),
    libtasn1 and HarfBuzz built for the host under the sanitizers from
    the sources the Makefile names, each fuzzer linked with
    tests/fuzz/onefile.c"""
    cflags = "CFLAGS=" + " ".join(SAN)
    driver = os.path.join(FUZZ, "onefile.c")
    binaries = {}

    webp = unpack("WEBP_TAR", workdir, "libwebp")
    must(["./configure", "--disable-shared", "--enable-static",
          "--disable-sse4.1", "--disable-avx2", "--disable-gl", "--disable-sdl",
          "--disable-png", "--disable-jpeg", "--disable-tiff", "--disable-gif",
          "--disable-wic", "--enable-libwebpdemux", cflags], "libwebp", cwd=webp)
    must(["make", "-j8", "-C", "sharpyuv"], "libwebp", cwd=webp)
    must(["make", "-j8", "-C", "src"], "libwebp", cwd=webp)
    b = os.path.join(workdir, "fz-wpe-webp")
    must(["gcc", *SAN, "-w", "-I", os.path.join(webp, "src"), "-o", b,
          os.path.join(FUZZ, "webp_harness.c"), driver,
          os.path.join(webp, "src", "demux", ".libs", "libwebpdemux.a"),
          os.path.join(webp, "src", ".libs", "libwebp.a"),
          os.path.join(webp, "sharpyuv", ".libs", "libsharpyuv.a"),
          "-lm", "-lpthread"], "the libwebp harness")
    binaries["wpe-webp"] = b

    asn = unpack("TASN1_TAR", workdir, "libtasn1")
    must(["./configure", "--disable-shared", "--enable-static", "--disable-doc",
          cflags], "libtasn1", cwd=asn)
    must(["make", "-j8", "-C", "lib"], "libtasn1", cwd=asn)
    for name, src in (("wpe-tasn1-pkix", "libtasn1_pkix_der_fuzzer.c"),
                      ("wpe-tasn1-der", "asn1_decode_simple_der_fuzzer.c")):
        b = os.path.join(workdir, "fz-" + name)
        must(["gcc", *SAN, "-w", "-I", asn,
              "-I", os.path.join(asn, "lib", "includes"),
              "-I", os.path.join(asn, "fuzz"), "-o", b,
              os.path.join(asn, "fuzz", src), driver,
              os.path.join(asn, "lib", ".libs", "libtasn1.a")],
             "libtasn1's " + src)
        binaries[name] = b

    hb = os.path.join(ROOT, "third_party", "harfbuzz", "src")
    obj = os.path.join(workdir, "harfbuzz.o")
    must(["g++", *SAN, "-w", "-fno-exceptions", "-fno-rtti", "-c",
          os.path.join(hb, "harfbuzz.cc"), "-o", obj], "HarfBuzz")
    b = os.path.join(workdir, "fz-wpe-harfbuzz")
    objs = []
    for src in (os.path.join(FUZZ, "harfbuzz_harness.c"), driver):
        objs.append(os.path.join(workdir, os.path.basename(src) + ".hb.o"))
        must(["gcc", *SAN, "-w", "-I", hb, "-c", src, "-o", objs[-1]],
             "the HarfBuzz harness")
    must(["g++", *SAN, "-o", b, *objs, obj], "the HarfBuzz harness")
    binaries["wpe-harfbuzz"] = b
    return binaries


def wpe_seeds(corpus):
    """The WebP pictures above; certificates in DER, the form the
    PKIX fuzzer takes them in; and fonts: the system's, and t_wpedeps's
    own with its ligatures, kerning, small capitals and marks"""
    sys.path.insert(0, os.path.join(ROOT, "scripts", "tests"))
    import t_wpedeps                       # noqa: E402
    fonts = [open(os.path.join(ROOT, "fonts", f), "rb").read()
             for f in sorted(os.listdir(os.path.join(ROOT, "fonts")))
             if f.endswith(".ttf")]
    fonts.append(t_wpedeps.make_font())
    return {
        "wpe-webp": list(corpus.get("webp", [])),
        "wpe-tasn1-pkix": list(corpus["x509"]),
        "wpe-tasn1-der": [b"\x03\x05\x00" + b"wpe!", b"\x04\x03abc",
                          b"\x04\x81\x80" + bytes(128)] + list(corpus["x509"])[:4],
        "wpe-harfbuzz": fonts,
    }


# The libraries under the web engine's network process (Makefile, 'The
# network'): OpenSSL's own fuzzers - certificates, ASN.1, PEM and both
# ends of a TLS handshake - ours for nghttp2, whose archive has none,
# libpsl's and libsoup's own
OPENSSL_FUZZERS = ("x509", "asn1", "pem", "client", "server")
LIBPSL_FUZZERS = ("libpsl_fuzzer", "libpsl_load_fuzzer",
                  "libpsl_load_dafsa_fuzzer")
SOUP_FUZZERS = ("cookie_parse", "date_time", "decode_data_uri",
                "header_parsing", "headers_parse_request",
                "headers_parse_response", "content_sniffer")


def build_net(workdir):
    """OpenSSL, nghttp2, libpsl and libsoup built for the host under the
    sanitizers from the archives the Makefile names, libsoup against
    build_glib()'s GLib and the other two installed beside it; each
    fuzzer linked with tests/fuzz/onefile.c"""
    driver = os.path.join(FUZZ, "onefile.c")
    root = os.path.join(workdir, "net-root")
    binaries = {}

    ssl = unpack("OPENSSL_TAR", workdir, "openssl")
    # (as OSS-Fuzz builds it: FUZZING_BUILD_MODE lets a handshake past
    # the checks a fuzzer cannot satisfy, and its assembly reads words
    # at any alignment, on purpose)
    must(["./Configure", "linux-x86_64", "no-shared", "no-tests", "no-docs",
          "no-apps", "enable-asan", "enable-ubsan", "-DPEDANTIC",
          "-DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION",
          "-fno-sanitize=alignment", "CC=gcc"], "OpenSSL", cwd=ssl)
    must(["make", "-j8", "build_libs"], "OpenSSL", cwd=ssl)
    fz = os.path.join(ssl, "fuzz")
    for name in OPENSSL_FUZZERS:
        b = os.path.join(workdir, "fz-openssl-" + name)
        must(["gcc", *SAN, "-fno-sanitize=alignment",
              "-DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION", "-w",
              "-I", os.path.join(ssl, "include"), "-I", fz, "-o", b,
              os.path.join(fz, name + ".c"), os.path.join(fz, "fuzz_rand.c"),
              os.path.join(FUZZ, "openssl_shim.c"), driver,
              os.path.join(ssl, "libssl.a"), os.path.join(ssl, "libcrypto.a"),
              "-lpthread", "-ldl"], "OpenSSL's fuzz " + name)
        binaries["openssl-" + name] = b

    h2 = unpack("NGHTTP2_TAR", workdir, "nghttp2")
    must(["./configure", "--disable-shared", "--enable-static",
          "--enable-lib-only", "--prefix=" + root, "--libdir=" + root + "/lib",
          "CFLAGS=" + " ".join(SAN)], "nghttp2", cwd=h2)
    must(["make", "-j8", "-C", "lib", "install"], "nghttp2", cwd=h2)
    b = os.path.join(workdir, "fz-nghttp2")
    must(["gcc", *SAN, "-I", os.path.join(root, "include"), "-o", b,
          os.path.join(FUZZ, "nghttp2_harness.c"), driver,
          os.path.join(root, "lib", "libnghttp2.a")], "the nghttp2 harness")
    binaries["nghttp2"] = b

    psl = unpack("LIBPSL_TAR", workdir, "libpsl")
    must(["meson", "setup", "build", "--wrap-mode=nodownload",
          "--prefix=" + root, "--libdir=lib", "--buildtype=debugoptimized",
          "-Ddefault_library=static", "-Db_sanitize=address,undefined",
          "-Druntime=no", "-Dbuiltin=true", "-Dtests=false", "-Ddocs=false"],
         "libpsl", cwd=psl)
    must(["ninja", "-C", "build", "install"], "libpsl", cwd=psl)
    for name in LIBPSL_FUZZERS:
        b = os.path.join(workdir, "fz-" + name)
        # (they include the config.h meson wrote; their own driver,
        # main.c, reads the corpus directories itself)
        must(["gcc", *SAN, "-w", "-I", os.path.join(psl, "build"),
              "-I", os.path.join(psl, "include"),
              "-I", os.path.join(psl, "fuzz"), "-o", b,
              os.path.join(psl, "fuzz", name + ".c"), driver,
              os.path.join(root, "lib", "libpsl.a")], "libpsl's " + name)
        binaries[name.replace("_fuzzer", "").replace("_", "-")] = b

    soup = unpack("LIBSOUP_TAR", workdir, "libsoup")
    env = dict(os.environ, PKG_CONFIG_PATH=":".join(
        [os.path.join(root, "lib", "pkgconfig"),
         os.path.join(workdir, "glib-root", "lib", "pkgconfig")]))
    must(["meson", "setup", "build", "--wrap-mode=nodownload",
          "--prefix=" + root, "--libdir=lib", "--buildtype=debugoptimized",
          "-Ddefault_library=static", "-Db_sanitize=address,undefined",
          "-Dgssapi=disabled", "-Dntlm=disabled", "-Dbrotli=disabled",
          "-Dzstd=disabled", "-Dtls_check=false", "-Dintrospection=disabled",
          "-Dvapi=disabled", "-Ddocs=disabled", "-Ddoc_tests=false",
          "-Dtests=false", "-Dautobahn=disabled", "-Dinstalled_tests=false",
          "-Dsysprof=disabled", "-Dfuzzing=disabled",
          "-Dpkcs11_tests=disabled"], "libsoup", cwd=soup, env=env)
    must(["ninja", "-C", "build", "install"], "libsoup", cwd=soup, env=env)
    flags = subprocess.run(["pkg-config", "--static", "--cflags", "--libs",
                            "libsoup-3.0"], env=env, capture_output=True,
                           text=True, check=True).stdout.split()
    fz = os.path.join(soup, "fuzzing")
    for name in SOUP_FUZZERS:
        b = os.path.join(workdir, "fz-soup-" + name)
        must(["gcc", *SAN, "-DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION",
              "-w", "-I", fz, "-o", b, os.path.join(fz, "fuzz_%s.c" % name),
              driver, *flags], "libsoup's fuzz_" + name)
        binaries["soup-" + name.replace("_", "-")] = b
    return binaries, {"psl": os.path.join(psl, "fuzz"), "soup": soup}


def h2_frame(kind, flags, stream, payload):
    return struct.pack(">I", len(payload))[1:] + bytes([kind, flags]) + \
        struct.pack(">I", stream) + payload


def tls_flights(workdir):
    """What both ends of a TLS handshake send first, as Python's ssl
    (the host's OpenSSL, a separate build) writes it: the client's
    hellos for OpenSSL's server fuzzer and the server's answers for its
    client fuzzer, in TLS 1.3 and 1.2, with RSA and ECDSA certificates
    from seeds()"""
    import ssl
    hellos, answers = [], []
    for tag in ("rsa", "p256"):
        for top in (ssl.TLSVersion.TLSv1_3, ssl.TLSVersion.TLSv1_2):
            srv = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            srv.load_cert_chain(os.path.join(workdir, tag + ".pem"),
                                os.path.join(workdir, tag + ".key"))
            cli = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            cli.check_hostname = False
            cli.verify_mode = ssl.CERT_NONE
            cli.maximum_version = top
            cli.set_alpn_protocols(["h2", "http/1.1"])
            pipes = [ssl.MemoryBIO() for _ in range(4)]
            c = cli.wrap_bio(pipes[0], pipes[1], server_hostname="localhost")
            s = srv.wrap_bio(pipes[2], pipes[3], server_side=True)
            try:
                c.do_handshake()
            except ssl.SSLWantReadError:
                pass
            hello = pipes[1].read()
            pipes[2].write(hello)
            try:
                s.do_handshake()
            except ssl.SSLWantReadError:
                pass
            hellos.append(hello)
            answers.append(pipes[3].read())
    return hellos, answers


def net_seeds(workdir, corpus, dirs):
    """Certificates, keys and signatures from seeds() for OpenSSL's
    parsers and TLS flights for its handshakes; HTTP/2 frames as a
    server and a client send them; libpsl's and libsoup's corpora from
    their archives"""
    def files(d, limit=20000):
        if not os.path.isdir(d):
            return []
        return [open(os.path.join(d, f), "rb").read()
                for f in sorted(os.listdir(d))
                if os.path.isfile(os.path.join(d, f))
                and os.path.getsize(os.path.join(d, f)) <= limit]
    pem = [open(os.path.join(workdir, t + ext), "rb").read()
           for t in ("rsa", "p256", "p384") for ext in (".pem", ".key")]
    hellos, answers = tls_flights(workdir)

    settings = h2_frame(4, 0, 0, struct.pack(">HIHI", 3, 100, 4, 65535))
    ack = h2_frame(4, 1, 0, b"")
    # RFC 7541's examples: a response (C.6.1, Huffman-coded) and a
    # request (C.4.1); then indexed, literal and a table size update
    response = bytes.fromhex(
        "488264025885aec3771a4b6196d07abe941054d444a8200595040b8166e082a6"
        "2d1bff6e919d29ad171863c78f0b97c8e9ae82ae43d3")
    request = bytes.fromhex("828684418cf1e3c2e5f23a6ba0ab90f4ff")
    server = (settings + ack +
              h2_frame(1, 4, 1, response) +
              h2_frame(0, 8, 1, b"\x03hello, world\x00\x00\x00") +
              h2_frame(0, 1, 1, b"!") +
              h2_frame(8, 0, 0, struct.pack(">I", 1 << 20)) +
              h2_frame(6, 0, 0, b"12345678") +
              h2_frame(5, 4, 1, struct.pack(">I", 2) + b"\x88") +
              h2_frame(7, 0, 0, struct.pack(">II", 1, 0)))
    split = (settings + h2_frame(1, 0, 1, b"\x3f\xe1\x1f\x88") +
             h2_frame(9, 4, 1, b"\x5f\x09text/html") +
             h2_frame(3, 0, 1, struct.pack(">I", 8)))
    client = (settings + ack +
              h2_frame(1, 4 | 1, 1, request) +
              h2_frame(1, 4 | 0x20, 3, struct.pack(">IB", 1, 15) + request) +
              h2_frame(0, 1, 3, b"posted") +
              h2_frame(2, 0, 3, struct.pack(">IB", 0, 200)) +
              h2_frame(3, 0, 3, struct.pack(">I", 8)))

    psl = {name: files(os.path.join(dirs["psl"], name + ".in"))
           for name in LIBPSL_FUZZERS}
    soup = {name: files(os.path.join(dirs["soup"], "fuzzing", "corpus",
                                     "fuzz_" + name))
            for name in SOUP_FUZZERS}
    soup["content_sniffer"] = files(os.path.join(dirs["soup"], "tests",
                                                 "resources"))
    out = {
        "openssl-x509": list(corpus["x509"]),
        "openssl-asn1": (list(corpus["x509"]) + list(corpus["ecdsa"]) +
                         list(corpus["mbedtls-privkey"]) +
                         list(corpus["mbedtls-pubkey"])),
        "openssl-pem": pem,
        "openssl-client": answers,
        "openssl-server": hellos,
        "nghttp2": [server, split, client],
    }
    for name, seeds in psl.items():
        out[name.replace("_fuzzer", "").replace("_", "-")] = seeds
    for name, seeds in soup.items():
        out["soup-" + name.replace("_", "-")] = seeds
    return {k: v for k, v in out.items() if v}


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


def inet_sum(data):
    """The Internet checksum (RFC 1071) of some bytes"""
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def frames():
    """Valid Ethernet frames: ARP, ICMP, UDP and TCP - checksums and
    all, since the kernel drops a frame whose checksums do not add up
    before it looks any further"""
    mac_us = bytes([0x52, 0x54, 0x00, 0x12, 0x34, 0x56])
    mac_them = bytes([0x52, 0x55, 0x0a, 0x00, 0x02, 0x02])
    ip_us = bytes([10, 0, 2, 15])
    ip_them = bytes([10, 0, 2, 2])

    def eth(kind, payload):
        return mac_us + mac_them + kind + payload

    def ip(proto, payload):
        total = 20 + len(payload)
        if proto in (6, 17):
            pseudo = ip_them + ip_us + bytes([0, proto]) + \
                len(payload).to_bytes(2, "big")
            at = 16 if proto == 6 else 6
            c = inet_sum(pseudo + payload)
            payload = payload[:at] + c.to_bytes(2, "big") + payload[at + 2:]
        elif proto == 1:
            c = inet_sum(payload)
            payload = payload[:2] + c.to_bytes(2, "big") + payload[4:]
        hdr = (bytes([0x45, 0x00]) + total.to_bytes(2, "big") +
               b"\x00\x01\x00\x00\x40" + bytes([proto]) + b"\x00\x00" +
               ip_them + ip_us)
        hdr = hdr[:10] + inet_sum(hdr).to_bytes(2, "big") + hdr[12:]
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
           "inflate": [], "jpeg": [], "cookie": [], "gif": [], "png": [],
           "svg": [], "webp": [],
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
    out["png"] += [p for p, _ in png_samples()]

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


def png_samples():
    """PNGs written by Pillow, with what Pillow reads back from each: all
    five colour kinds - grey, grey with alpha, RGB, RGBA, a palette with
    a transparent entry - in pictures whose rows need every filter, as
    (file, (w, h, rgba bytes))"""
    from PIL import Image
    import io
    out = []
    w, h = 37, 23
    base = Image.new("RGBA", (w, h))
    base.putdata([((x * 7) & 255, (y * 11) & 255, (x * y) & 255,
                   (x * 13 + y * 5) & 255) for y in range(h) for x in range(w)])
    pics = []
    for mode in ("L", "LA", "RGB", "RGBA"):
        pics.append((base.convert(mode), {}))
    # palettes of 2, 4, 16 and 256 colours: Pillow writes them at 1, 2,
    # 4 and 8 bits a pixel; and grey at one bit
    for colours in (2, 4, 16, 256):
        pal = base.convert("RGB").quantize(colors=colours)
        pal.info["transparency"] = 1
        pics.append((pal, {"bits": max(1, (colours - 1).bit_length())}))
    pics.append((base.convert("1"), {}))
    for pic, extra in pics:
        b = io.BytesIO()
        pic.save(b, "PNG", optimize=False, **extra)
        data = b.getvalue()
        back = Image.open(io.BytesIO(data)).convert("RGBA")
        out.append((data, (w, h, back.tobytes())))
    # Sixteen bits a channel, RGBA, made here: Pillow reads such files
    # by clipping rather than scaling, so what is wanted is known from
    # the making instead - the top byte of every channel - and every
    # row has another of the five filters
    import struct
    import zlib as z

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body +
                struct.pack(">I", z.crc32(kind + body) & 0xFFFFFFFF))
    rows, want = b"", bytearray()
    prev = bytes(w * 8)
    for y in range(h):
        raw = bytearray()
        for x in range(w):
            for c in range(4):
                v = ((x * 1031 + y * 517 + c * 9973) * 2654435761) & 0xFFFF
                raw += struct.pack(">H", v)
                want.append(v >> 8)
        f = y % 5
        line = bytearray(raw)
        for i in range(len(raw)):
            a = raw[i - 8] if i >= 8 else 0
            b, c = prev[i], (prev[i - 8] if i >= 8 else 0)
            if f == 1:
                line[i] = (raw[i] - a) & 255
            elif f == 2:
                line[i] = (raw[i] - b) & 255
            elif f == 3:
                line[i] = (raw[i] - ((a + b) >> 1)) & 255
            elif f == 4:
                p_ = a + b - c
                pa, pb, pc = abs(p_ - a), abs(p_ - b), abs(p_ - c)
                pred = a if pa <= pb and pa <= pc else b if pb <= pc else c
                line[i] = (raw[i] - pred) & 255
        rows += bytes([f]) + bytes(line)
        prev = bytes(raw)
    data = (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 16, 6, 0, 0, 0)) +
            chunk(b"IDAT", z.compress(rows, 9)) + chunk(b"IEND", b""))
    out.append((data, (w, h, bytes(want))))
    return out


def png_pixels(binary):
    """Every PNG of png_samples() through the decoder: the pixels that
    came out different from Pillow's"""
    wrong = []
    for data, (w, h, rgba) in png_samples():
        path = os.path.join(os.path.dirname(binary), "sample.png")
        with open(path, "wb") as f:
            f.write(data)
        run = subprocess.run([binary, "pngpixels", path], capture_output=True,
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
        gbinaries, gfuzz = build_glib(workdir)
        mbinaries.update(gbinaries)
        mbinaries.update(build_media(workdir))
        mbinaries.update(build_wpe(workdir))
        mbinaries.update(build_gst(workdir))
        obinaries, oseeds = build_opus(workdir)
        mbinaries.update(obinaries)
        nbinaries, ndirs = build_net(workdir)
        mbinaries.update(nbinaries)
        hbinary = build_hid(workdir)
        mbinaries["wpa"] = build_wpa(workdir)
        mbinaries["wlan"] = build_wlan(workdir)
        mbinaries["tfs4"] = build_tfs4(workdir)
        fbinary = build_fdt(workdir)
        corpus = seeds(workdir)
        corpus["frames"] = frames()
        corpus["ttf"] = fonts()
        corpus["hid"] = hid_seeds()
        corpus["wpa"] = wpa_seeds()
        corpus["wlan"] = wlan_seeds()
        corpus["tfs4"] = tfs4_seeds(workdir)
        corpus["fdt"] = [t for _, t, want in FDT_TREES
                         if want.startswith("r=0")]
        corpus["mbedtls-x509crt"] = list(corpus["x509"])
        corpus["mbedtls-pubkey"], corpus["mbedtls-privkey"] = \
            mbedtls_seeds(workdir)
        corpus.update(glib_seeds(gfuzz))
        corpus.update(media_seeds(corpus))
        corpus.update(wpe_seeds(corpus))
        corpus.update(gst_seeds(workdir))
        corpus.update(oseeds)
        corpus.update(net_seeds(workdir, corpus, ndirs))
        print("the harness is built, cases per parser: %d" % CASES)
        wrong = fdt_layouts(fbinary, workdir)
        print("  device trees against what they were made with: %d wrong"
              % len(wrong))
        for name, got, want in wrong:
            failures.append(("fdtlayout", name,
                             "got  %s\nwant %s" % (got, want)))
        wrong = tfs4_e2fsck(mbinaries["tfs4"], workdir, corpus["tfs4"])
        print("  TFS4's writing against e2fsck: %d wrong" % len(wrong))
        for what, why in wrong:
            failures.append(("tfs4e2fsck", what, why))
        bad = gif_pixels(binary)
        print("  GIFs against Pillow's reading of them: %d wrong" % len(bad))
        for b in bad:
            failures.append(("gifpixels", b, "the pixels differ"))
        bad = png_pixels(binary)
        print("  PNGs against Pillow's reading of them: %d wrong" % len(bad))
        for b in bad:
            failures.append(("pngpixels", b, "the pixels differ"))
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
