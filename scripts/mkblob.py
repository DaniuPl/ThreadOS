#!/usr/bin/env python3
"""Known bytes for libc/tests/bigprog.c (scripts/mkblob.py)

    python3 scripts/mkblob.py SEED SIZE OUT

SIZE bytes, each 32 of them the SHA-256 of the seed and the block's
number: random to look at, and the same every time, so that
scripts/tests/t_bigprog.py can make them again on the host.
"""
import hashlib
import sys


def blob(seed, size):
    out = bytearray()
    i = 0
    while len(out) < size:
        out += hashlib.sha256(f"{seed}:{i}".encode()).digest()
        i += 1
    return bytes(out[:size])


def main():
    seed, size, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    with open(path, "wb") as fh:
        fh.write(blob(seed, size))


if __name__ == "__main__":
    main()
