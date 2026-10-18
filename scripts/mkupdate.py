#!/usr/bin/env python3
"""Makes update.txt - the signed description of a ThreadOS release.

    mkupdate.py <out> <version> <key.pem> <kernel> <initrd> <loader>

The format is what user/update.c reads:

    ThreadOS update
    version 42
    kernel threados64.bin <size> <sha256>
    initrd initrd64.img <size> <sha256>
    loader BOOTX64.EFI <size> <sha256>
    signature <r||s, 64 bytes in hex>

The signature is ECDSA P-256 over SHA-256 of every byte before the
signature line, by the key in key.pem (the release workflow takes it
from a repository secret; the tests make one of their own). It is made
by the openssl command, so nothing beyond Python and OpenSSL is needed -
which is what a CI runner has. The file names are the release's asset
names, so update.c fetches them from next to update.txt.
"""

import hashlib
import os
import subprocess
import sys

NAMES = ("threados64.bin", "initrd64.img", "BOOTX64.EFI")


def der_to_rs(der):
    """An ECDSA signature in DER (SEQUENCE { INTEGER r, INTEGER s }) as
    32 bytes of r and 32 of s"""
    def integer(at):
        assert der[at] == 0x02
        n = der[at + 1]
        v = der[at + 2:at + 2 + n]
        return int.from_bytes(v, "big"), at + 2 + n
    assert der[0] == 0x30
    at = 2 if der[1] < 0x80 else 2 + (der[1] & 0x7F)
    r, at = integer(at)
    s, _ = integer(at)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def manifest(version, files):
    lines = ["ThreadOS update", "version %d" % version]
    for kind, name, path in zip(("kernel", "initrd", "loader"), NAMES, files):
        data = open(path, "rb").read()
        lines.append("%s %s %d %s" % (kind, name, len(data),
                                      hashlib.sha256(data).hexdigest()))
    return ("\n".join(lines) + "\n").encode()


def sign(body, key):
    der = subprocess.run(["openssl", "dgst", "-sha256", "-sign", key],
                         input=body, capture_output=True, check=True).stdout
    return der_to_rs(der)


def main():
    if len(sys.argv) != 7:
        print(__doc__)
        return 2
    out, version, key = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    files = sys.argv[4:7]
    body = manifest(version, files)
    sig = sign(body, key)
    with open(out, "wb") as fh:
        fh.write(body + b"signature " + sig.hex().encode() + b"\n")
    print("update.txt: version %d, %s" % (version, ", ".join(
        "%s %d B" % (n, os.path.getsize(f)) for n, f in zip(NAMES, files))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
