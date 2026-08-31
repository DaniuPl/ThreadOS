#!/usr/bin/env python3
"""Packs files into a ProtonOS initrd (the PIRD format).

Format: "PIRD" | u32 count | entries {name[16], u32 offset, u32 size} | data

Usage: mkinitrd.py output.img name=file [name=file ...]
"""

import struct
import sys


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    out_path = sys.argv[1]
    files = []
    for spec in sys.argv[2:]:
        name, _, path = spec.partition("=")
        if not path:
            print(f"blad: '{spec}' nie jest w formie nazwa=plik")
            return 1
        if len(name.encode()) > 15:
            print(f"blad: nazwa '{name}' za dluga (max 15 znakow)")
            return 1
        files.append((name, open(path, "rb").read()))

    header_size = 8 + 24 * len(files)
    out = bytearray(b"PIRD")
    out += struct.pack("<I", len(files))

    offset = header_size
    for name, data in files:
        out += struct.pack("<16sII", name.encode(), offset, len(data))
        offset += len(data)
    for _, data in files:
        out += data

    with open(out_path, "wb") as f:
        f.write(out)
    print(f"initrd: {len(files)} plikow, {len(out)} B -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
