#!/usr/bin/env python3
"""mktapp.py - a ThreadOS application package (.tapp)

A .tapp is an image in the initrd's own format (TIRD, mkinitrd.py),
so the kernel takes one apart with the code it already trusts. Its
entries, in this order:

  manifest   text: a first line "ThreadOS app", then "key value" lines
  ...        the package's programs and data, under the names the
             system runs and reads them by (at most 15 characters)
  signature  64 bytes, the last entry and the last bytes of the file:
             ECDSA P-256 over the SHA-256 of every byte before them,
             r and s of 32 bytes each

The manifest says:

  id       the package's name: a-z, 0-9 and '-', at most 15
           characters; installed, the file is <id>.tapp
  name     what people see, name.pl in Polish
  version  a whole number, larger for a newer package
  arch     the processor its programs are for: x86_64 or aarch64
  exec     the program that starts it, one of the package's entries
  art      the dock's picture for it (kernel/desktop.c ART_*), or the
           first letter of its name when there is none
  dock     "yes" puts it in the dock

The signature is made as an update's is (mkupdate.py), with the key
that signs releases; a package without one can be loaded by a loader
that finds it on the EFI partition - which is as trusted as the kernel
on it - but 'tapp verify' says so. The hashed bytes include the table
that says where the signature is, so it cannot be moved or dropped
without the hash changing.

Usage:
  mktapp.py OUT.tapp --id ID --name NAME --version N --arch ARCH
            --exec PROGRAM [--name-pl NAME] [--art N] [--dock]
            [--key KEY.pem] NAME=FILE ...
  mktapp.py --info PKG.tapp
  mktapp.py --verify PKG.tapp PUBKEY_HEX     (65 bytes: 04 || X || Y)
"""

import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkupdate import der_to_rs  # noqa: E402

MAGIC = "ThreadOS app"
ARCHES = ("x86_64", "aarch64")
SIG_LEN = 64


def pack(entries):
    """A TIRD image of (name, bytes), in order"""
    out = bytearray(b"TIRD" + struct.pack("<I", len(entries)))
    offset = 8 + 24 * len(entries)
    for name, data in entries:
        out += struct.pack("<16sII", name.encode(), offset, len(data))
        offset += len(data)
    for _, data in entries:
        out += data
    return bytes(out)


def unpack(image):
    """The entries of a TIRD image, checked as the kernel checks them"""
    if image[:4] != b"TIRD":
        raise ValueError("not a TIRD image")
    count = struct.unpack_from("<I", image, 4)[0]
    if count > (len(image) - 8) // 24:
        raise ValueError("a listing longer than the image")
    out = []
    for i in range(count):
        raw, off, size = struct.unpack_from("<16sII", image, 8 + 24 * i)
        if b"\0" not in raw or off > len(image) or size > len(image) - off:
            raise ValueError("entry %d lies outside the image" % i)
        out.append((raw.split(b"\0")[0].decode(), off, size))
    return out


def manifest_text(a):
    lines = [MAGIC, "id " + a.id, "name " + a.name]
    if a.name_pl:
        lines.append("name.pl " + a.name_pl)
    lines += ["version %d" % a.version, "arch " + a.arch, "exec " + a.exec]
    if a.art is not None:
        lines.append("art %d" % a.art)
    if a.dock:
        lines.append("dock yes")
    return ("\n".join(lines) + "\n").encode()


def parse_manifest(text):
    lines = text.decode("utf-8").split("\n")
    if lines[0] != MAGIC:
        raise ValueError("the manifest does not start with %r" % MAGIC)
    keys = {}
    for line in lines[1:]:
        if line:
            k, _, v = line.partition(" ")
            keys[k] = v
    return keys


def sign(body, key):
    der = subprocess.run(["openssl", "dgst", "-sha256", "-sign", key],
                         input=body, capture_output=True, check=True).stdout
    return der_to_rs(der)


def build(a):
    if not re.fullmatch(r"[a-z0-9-]{1,15}", a.id):
        raise SystemExit("error: id is a-z, 0-9 and '-', 1 to 15 of them")
    if a.arch not in ARCHES:
        raise SystemExit("error: arch is one of " + ", ".join(ARCHES))
    files = []
    for spec in a.files:
        name, _, path = spec.partition("=")
        if not path or not 0 < len(name.encode()) <= 15 or \
                name in ("manifest", "signature"):
            raise SystemExit("error: '%s' is not NAME=FILE with a name of "
                             "1 to 15 characters of one's own" % spec)
        files.append((name, open(path, "rb").read()))
    if a.exec not in [n for n, _ in files]:
        raise SystemExit("error: exec '%s' is not one of the package's "
                         "files" % a.exec)
    entries = [("manifest", manifest_text(a))] + files + \
              [("signature", b"\0" * SIG_LEN)]
    image = pack(entries)
    body = image[:-SIG_LEN]
    sig = sign(body, a.key) if a.key else b"\0" * SIG_LEN
    with open(a.out, "wb") as fh:
        fh.write(body + sig)
    print("%s: %s %d, %d files, %d B, %s" % (
        a.out, a.id, a.version, len(files), len(image),
        "signed" if a.key else "NOT signed"))


def info(path):
    image = open(path, "rb").read()
    entries = unpack(image)
    if not entries or entries[0][0] != "manifest":
        raise SystemExit("error: the first entry is not the manifest")
    _, off, size = entries[0]
    for k, v in parse_manifest(image[off:off + size]).items():
        print("%s: %s" % (k, v))
    for name, off, size in entries[1:]:
        print("  %-15s %d B" % (name, size))


def verify(path, pubhex):
    """The signature against a public key, by OpenSSL - an
    implementation other than the system's own (user/uecdsa.h)"""
    image = open(path, "rb").read()
    entries = unpack(image)
    last = entries[-1] if entries else None
    if not last or last[0] != "signature" or last[2] != SIG_LEN or \
            last[1] + SIG_LEN != len(image):
        print("no signature")
        return 1
    r = int.from_bytes(image[-64:-32], "big")
    s = int.from_bytes(image[-32:], "big")

    def der_int(v):
        b = v.to_bytes(33, "big").lstrip(b"\0")
        if not b or b[0] & 0x80:
            b = b"\0" + b
        return b"\x02" + bytes([len(b)]) + b
    seq = der_int(r) + der_int(s)
    der = b"\x30" + bytes([len(seq)]) + seq
    # SubjectPublicKeyInfo for an uncompressed P-256 point
    spki = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107"
                         "034200") + bytes.fromhex(pubhex)
    with tempfile.TemporaryDirectory() as tmp:
        pem = os.path.join(tmp, "pub.der")
        sig = os.path.join(tmp, "sig.der")
        open(pem, "wb").write(spki)
        open(sig, "wb").write(der)
        ok = subprocess.run(["openssl", "dgst", "-sha256", "-verify", pem,
                             "-keyform", "DER", "-signature", sig],
                            input=image[:-SIG_LEN],
                            capture_output=True).returncode == 0
    print("signed by that key" if ok else "NOT signed by that key")
    return 0 if ok else 1


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--info":
        info(sys.argv[2])
        return 0
    if len(sys.argv) == 4 and sys.argv[1] == "--verify":
        return verify(sys.argv[2], sys.argv[3])
    p = argparse.ArgumentParser(usage=__doc__)
    p.add_argument("out")
    p.add_argument("--id", required=True)
    p.add_argument("--name", required=True)
    p.add_argument("--name-pl", dest="name_pl")
    p.add_argument("--version", type=int, required=True)
    p.add_argument("--arch", required=True)
    p.add_argument("--exec", required=True)
    p.add_argument("--art", type=int)
    p.add_argument("--dock", action="store_true")
    p.add_argument("--key")
    p.add_argument("files", nargs="+")
    build(p.parse_args())
    return 0


if __name__ == "__main__":
    sys.exit(main())
