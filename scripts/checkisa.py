#!/usr/bin/env python3
"""Holds a build to the first x86-64 processor's instruction set.

ThreadOS is built for any 64-bit PC (ARCH64 in the Makefile), and its
kernel enables SSE's registers but not AVX's. An instruction from a
later extension - AVX's VEX encoding, BMI2, MOVBE, SSE4 - is an Invalid
Opcode on a processor without it, and AVX ones are an Invalid Opcode on
every processor, since the kernel never turns their state on. A
compiler aiming higher by default (Ubuntu 26.04's, whose whole archive,
libgcc and libstdc++ included, is built for x86-64-v3) puts them
everywhere, and nothing at build time says so.

This disassembles the kernel and every program in the initrd and lists
what goes beyond the first x86-64. Two kinds of exception, each named
below with its reason: code that asks the processor (CPUID) before it
uses an extension, and tzcnt, which a processor without BMI1 runs as
bsf - the compiler emits it only where the two agree.

Usage: checkisa.py KERNEL INITRD   (exit status 1 when something is
beyond the baseline)
"""

import collections
import os
import re
import struct
import subprocess
import sys
import tempfile

# Mnemonics of instructions the first x86-64 processors lack: VEX and
# EVEX (AVX, AVX2, AVX-512, FMA, F16C - all spelled with a v; verr and
# verw are the 286's), BMI1/BMI2, LZCNT, POPCNT, MOVBE, SSE3, SSSE3,
# SSE4.1/4.2, AES-NI, PCLMULQDQ, SHA, ADX, XSAVE, RDRAND/RDSEED,
# CMPXCHG16B and the cache-line ones.
BEYOND = re.compile(
    r"^(v(?!err|erw)[a-z0-9]+|andn|bextr|blsi|blsmsk|blsr|bzhi|mulx|pdep"
    r"|pext|rorx|sarx|shlx|shrx|lzcnt|tzcnt|popcnt|movbe|addsubp[sd]"
    r"|haddp[sd]|hsubp[sd]|lddqu|movddup|movshdup|movsldup|fisttp[a-z]*"
    r"|pshufb|phadd[a-z]*|phsub[a-z]*|pmaddubsw|pmulhrsw|psign[bwd]"
    r"|pabs[bwd]|palignr|blendv?p[sd]|pblendvb|pblendw|dpp[sd]|extractps"
    r"|insertps|movntdqa|mpsadbw|packusdw|pcmpeqq|pextr[bdq]|pinsr[bdq]"
    r"|phminposuw|pmaxs[bd]|pmaxu[dw]|pmins[bd]|pminu[dw]|pmovsx[a-z]+"
    r"|pmovzx[a-z]+|pmuldq|pmulld|ptest|round[ps][sd]|pcmp[ei]str[im]"
    r"|pcmpgtq|crc32[a-z]?|aes[a-z]+|pclmul[a-z]*|sha1[a-z0-9]+"
    r"|sha256[a-z0-9]+|adcx|adox|xgetbv|xsave[a-z]*|xrstor[a-z]*|rdrand"
    r"|rdseed|cmpxchg16b|clflushopt|clwb)$")

PREFIXES = {"rex", "rex.W", "rex.B", "rex.R", "rex.X", "lock", "rep",
            "repz", "repnz", "repe", "repne", "notrack", "bnd", "data16",
            "addr32", "cs", "ds", "es", "ss", "fs", "gs"}

# tzcnt is rep bsf: without BMI1 the processor runs it as bsf, which
# differs only for a zero input, and GCC emits it only where the input
# cannot be zero
EVERYWHERE = {"tzcnt"}

# What asks the processor first, and why it may hold more
ASKS_FIRST = {
    "kernel": ("kernel/random.c: RDRAND and RDSEED behind CPUID",
               {"rdrand", "rdseed"}),
    "souptest": ("OpenSSL: its assembly has AES-NI, AVX2 and AVX-512 "
                 "paths chosen by OPENSSL_cpuid_setup", None),
    "curl": ("Mbed TLS: AES-NI behind mbedtls_aesni_has_support", None),
    "mbedtest": ("Mbed TLS: AES-NI behind mbedtls_aesni_has_support", None),
    "jsc": ("simdutf in WebKit: SSE4.2, AVX2 and AVX-512 kernels picked "
            "by CPUID and XGETBV at run time", None),
    "wpedeps": ("libgcrypt's src/hwf-x86.c: XGETBV only once CPUID's "
                "OSXSAVE bit says the system has it", {"xgetbv"}),
}


def beyond_baseline(path):
    """Counts of the instructions in an ELF file's code that the first
    x86-64 processor does not have"""
    out = subprocess.run(["objdump", "-d", "--no-show-raw-insn", path],
                         capture_output=True, text=True, check=True).stdout
    found = collections.Counter()
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) < 2 or not re.match(r"^\s*[0-9a-f]+:$", parts[0]):
            continue
        words = parts[1].split()
        while len(words) > 1 and words[0] in PREFIXES:
            words = words[1:]
        if words and BEYOND.match(words[0]):
            found[words[0]] += 1
    return found


def initrd_programs(initrd):
    """(name, bytes) of every ELF file in a TIRD initrd
    (scripts/mkinitrd.py)"""
    data = open(initrd, "rb").read()
    if data[:4] != b"TIRD":
        raise SystemExit(initrd + " is not an initrd")
    count = struct.unpack_from("<I", data, 4)[0]
    for i in range(count):
        name, off, size = struct.unpack_from("<16sII", data, 8 + 24 * i)
        blob = data[off:off + size]
        if blob[:4] == b"\x7fELF":
            yield name.rstrip(b"\0").decode(), blob


def check(kernel, initrd):
    """[(name, Counter of what is not allowed)] for the kernel and the
    initrd's programs, and [(name, reason)] of the exceptions used"""
    wrong, excused = [], []
    with tempfile.TemporaryDirectory() as tmp:
        files = [("kernel", kernel)]
        for name, blob in initrd_programs(initrd):
            path = os.path.join(tmp, name)
            with open(path, "wb") as f:
                f.write(blob)
            files.append((name, path))
        for name, path in files:
            found = beyond_baseline(path)
            for op in EVERYWHERE:
                found.pop(op, None)
            if name in ASKS_FIRST and found:
                reason, allowed = ASKS_FIRST[name]
                if allowed is None:
                    excused.append((name, reason))
                    continue
                for op in allowed:
                    found.pop(op, None)
                excused.append((name, reason))
            if found:
                wrong.append((name, found))
    return wrong, excused


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    wrong, excused = check(sys.argv[1], sys.argv[2])
    for name, reason in excused:
        print("  %-12s asks first: %s" % (name, reason))
    for name, found in wrong:
        top = ", ".join("%s %d" % kv for kv in found.most_common(6))
        print("BEYOND x86-64: %-12s %d instructions (%s)"
              % (name, sum(found.values()), top))
    if wrong:
        print("%d files need a processor newer than the first x86-64"
              % len(wrong))
        return 1
    print("Only the first x86-64 processor's instructions, outside code "
          "that asks first.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
