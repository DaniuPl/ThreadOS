#!/usr/bin/env python3
"""Packs files into a ThreadOS initrd (the TIRD format).

Format: "TIRD" | u32 count | entries {name[16], u32 offset, u32 size} | data

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
            print(f"error: '{spec}' is not in the form name=file")
            return 1
        if len(name.encode()) > 15:
            print(f"error: the name '{name}' is too long (max 15 characters)")
            return 1
        files.append((name, open(path, "rb").read()))

    header_size = 8 + 24 * len(files)
    out = bytearray(b"TIRD")
    out += struct.pack("<I", len(files))

    offset = header_size
    for name, data in files:
        out += struct.pack("<16sII", name.encode(), offset, len(data))
        offset += len(data)
    for _, data in files:
        out += data

    with open(out_path, "wb") as f:
        f.write(out)
    print(f"initrd: {len(files)} files, {len(out)} B -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
