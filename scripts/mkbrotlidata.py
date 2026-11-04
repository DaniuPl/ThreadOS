#!/usr/bin/env python3
"""Brotli's fixed data (third_party/brotli-rfc7932) as a C header for
user/ubrotli.h:

  python3 scripts/mkbrotlidata.py third_party/brotli-rfc7932 out.h
"""

import os
import sys

NAMES = (["Identity"] + [f"OmitLast{i}" for i in range(1, 10)] +
         ["UppercaseFirst", "UppercaseAll"] + [f"OmitFirst{i}" for i in range(1, 10)])


def c_bytes(name, data):
    rows = [", ".join(str(b) for b in data[i:i + 24]) for i in range(0, len(data), 24)]
    return f"static const unsigned char {name}[{len(data)}] = {{\n" + ",\n".join(rows) + "\n};\n"


def main():
    src, out = sys.argv[1], sys.argv[2]
    dictionary = open(os.path.join(src, "dictionary.bin"), "rb").read()
    context = open(os.path.join(src, "context.bin"), "rb").read()
    assert len(dictionary) == 122784 and len(context) == 2048
    # every prefix and suffix once, then the transforms as indexes to them
    strings, index, triples = b"", {}, []
    for line in open(os.path.join(src, "transforms.txt")):
        if line.startswith("#") or not line.strip():
            continue
        _, pre, kind, suf = line.split()
        ids = []
        for s in (pre, suf):
            raw = b"" if s == "-" else bytes.fromhex(s)
            if raw not in index:
                index[raw] = len(strings)
                strings += bytes([len(raw)]) + raw
            ids.append(index[raw])
        triples += [ids[0], NAMES.index(kind), ids[1]]
    assert len(triples) == 121 * 3
    with open(out, "w") as fh:
        fh.write("/* Made by scripts/mkbrotlidata.py from third_party/brotli-rfc7932 */\n")
        fh.write(c_bytes("brotli_dictionary", dictionary))
        fh.write(c_bytes("brotli_context", context))
        fh.write(c_bytes("brotli_affixes", strings))
        fh.write(f"static const unsigned short brotli_transforms[{len(triples)}] = {{\n" +
                 ", ".join(map(str, triples)) + "\n};\n")


if __name__ == "__main__":
    main()
