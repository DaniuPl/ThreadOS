#!/usr/bin/env python3
"""ThreadOS smoke test: the kernel's random numbers.

TLS keys are only as secret as the bytes they are made of. Programs
used to make their own: musl's getrandom() was a xorshift over the time
stamp counter, and user/urand.h a hash of the clock - guessable, and
the same in a forked child as in its parent. Now the kernel keeps a
pool (kernel/random.c): RDSEED and RDRAND when the processor has them,
and the cycle counter at every interrupt, hashed with BLAKE2s; the
output is ChaCha20 under a key replaced after every request.

Checked from outside:

  the kernel's own random.c, compiled on this machine with a small
  harness (scripts/tests/random/harness.c): its BLAKE2s agrees with
  Python's hashlib for inputs of every length up to a few blocks, fed
  in pieces of every size; its ChaCha20 with RFC 8439's example and a
  ChaCha20 written here (and with the cryptography package, when it is
  installed); and what random_bytes() hands out is exactly that
  keystream under the current key, with the next key the block before
  it - except when the pool is drawn on, when it is not

  on ThreadOS, randtest draws a megabyte through getrandom() in pieces
  of every size, and a quarter of one from /dev/urandom (which Mbed
  TLS reads, and which the C library makes up): the files it writes,
  read off the disk image, have as
  many ones as zeros, every byte value and pair of values about as
  often as chance allows, no 16-byte block twice, and zlib cannot
  shrink them

  a forked child and its parent, and four threads drawing at the same
  time, never get the same bytes

  two boots share nothing, and the processor's own generator is used
  when there is one (QEMU's "max" processor has both instructions,
  the default one neither)

Run on its own or through scripts/run_tests.py.
"""

import hashlib
import math
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, make_iso64, no64, TfsImage

HERE = os.path.dirname(os.path.abspath(__file__))
PROMPT = "thread64>"
M32 = 0xFFFFFFFF


# --- ChaCha20, from RFC 8439 ----------------------------------------------

def _rotl(x, n):
    return ((x << n) | (x >> (32 - n))) & M32


def _qr(s, a, b, c, d):
    s[a] = (s[a] + s[b]) & M32; s[d] = _rotl(s[d] ^ s[a], 16)
    s[c] = (s[c] + s[d]) & M32; s[b] = _rotl(s[b] ^ s[c], 12)
    s[a] = (s[a] + s[b]) & M32; s[d] = _rotl(s[d] ^ s[a], 8)
    s[c] = (s[c] + s[d]) & M32; s[b] = _rotl(s[b] ^ s[c], 7)


def chacha_block(key, counter, nonce):
    st = ([0x61707865, 0x3320646e, 0x79622d32, 0x6b206574] +
          list(struct.unpack("<8I", key)) + [counter] +
          list(struct.unpack("<3I", nonce)))
    x = st[:]
    for _ in range(10):
        _qr(x, 0, 4, 8, 12); _qr(x, 1, 5, 9, 13)
        _qr(x, 2, 6, 10, 14); _qr(x, 3, 7, 11, 15)
        _qr(x, 0, 5, 10, 15); _qr(x, 1, 6, 11, 12)
        _qr(x, 2, 7, 8, 13); _qr(x, 3, 4, 9, 14)
    return struct.pack("<16I", *((x[i] + st[i]) & M32 for i in range(16)))


RFC8439_KEY = bytes(range(32))
RFC8439_NONCE = bytes.fromhex("000000090000004a00000000")
RFC8439_BLOCK = bytes.fromhex(
    "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
    "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e")


def cryptography_block(key, counter, nonce):
    """The cryptography package's ChaCha20, when it is installed"""
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms
    except ImportError:
        return None
    c = Cipher(algorithms.ChaCha20(key, struct.pack("<I", counter) + nonce),
               mode=None)
    return c.encryptor().update(bytes(64))


def keystream(key, nonce, n):
    out = b""
    counter = 1
    while len(out) < n:
        out += chacha_block(key, counter, nonce)
        counter += 1
    return out[:n]


# --- The kernel's source on this machine ----------------------------------

class Harness:
    def __init__(self, work):
        self.exe = os.path.join(work, "harness")
        r = subprocess.run(["gcc", "-O1", "-w", "-o", self.exe,
                            os.path.join(HERE, "random", "harness.c")])
        self.p = None
        if r.returncode == 0:
            self.p = subprocess.Popen([self.exe], stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, text=True)
            self.banner = self.p.stdout.readline().strip()

    def ask(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        return self.p.stdout.readline().strip()

    def close(self):
        if self.p:
            self.p.stdin.close()
            self.p.wait(timeout=10)


def hx(b):
    return b.hex() if b else "-"


def host_checks(work):
    h = Harness(work)
    check("kernel/random.c builds on this machine with the harness",
          h.p is not None)
    if h.p is None:
        return
    try:
        check(f"and its self-test passes there too ({h.banner})",
              "match RFC 7693 and 8439" in h.banner)

        rnd = random.Random(1)
        lengths = list(range(0, 260)) + [511, 512, 513, 1000, 4096]
        wrong = []
        for n in lengths:
            data = bytes(rnd.getrandbits(8) for _ in range(n))
            want = hashlib.blake2s(data).hexdigest()
            for split in (1, 7, 63, 64, 65, 4096):
                got = h.ask(f"b2s {split} {hx(data)}")
                if got != want:
                    wrong.append((n, split))
        check(f"BLAKE2s: Python's hashlib agrees for {len(lengths)} lengths, "
              f"each fed in six piece sizes ({len(wrong)} wrong: "
              f"{wrong[:5]})", not wrong)

        check("ChaCha20 written here matches RFC 8439's example block",
              chacha_block(RFC8439_KEY, 1, RFC8439_NONCE) == RFC8439_BLOCK)
        check("and the kernel's ChaCha20 matches it",
              h.ask(f"chacha {hx(RFC8439_KEY)} 1 {hx(RFC8439_NONCE)}") ==
              RFC8439_BLOCK.hex())
        wrong = 0
        compared = 0
        for _ in range(200):
            key = rnd.randbytes(32)
            nonce = rnd.randbytes(12)
            counter = rnd.getrandbits(32)
            got = h.ask(f"chacha {hx(key)} {counter} {hx(nonce)}")
            want = chacha_block(key, counter, nonce)
            other = cryptography_block(key, counter, nonce)
            if other is not None:
                compared += 1
                wrong += other != want
            wrong += got != want.hex()
        check(f"for 200 random keys, nonces and counters too "
              f"({wrong} wrong; {compared} also checked with the "
              f"cryptography package)", wrong == 0)

        # What random_bytes() hands out
        sizes = [0, 1, 31, 32, 33, 63, 64, 65, 128, 1000, 4096] * 7
        modelled = pooled = 0
        bad = []
        outputs = set()
        for i, n in enumerate(sizes):
            if i == 40:
                h.ask("irq 64")      # enough interrupts for a reseed
            m = re.match(r"pool (\d) request (\d+) key (\S+) next (\S+) "
                         r"out (\S+)$", h.ask(f"draw {n}"))
            if not m:
                bad.append((i, "no answer"))
                continue
            pool, req = int(m.group(1)), int(m.group(2))
            key, nxt = bytes.fromhex(m.group(3)), bytes.fromhex(m.group(4))
            out = b"" if m.group(5) == "-" else bytes.fromhex(m.group(5))
            nonce = struct.pack("<II", req & M32, req >> 32) + bytes(4)
            if len(out) >= 32:
                outputs.add(out[:32])
            if pool:
                pooled += 1
                # a reseed changes the key first: the old key's stream
                # must not come out
                if n and out == keystream(key, nonce, n):
                    bad.append((i, "the pool changed nothing"))
                continue
            modelled += 1
            if out != keystream(key, nonce, n):
                bad.append((i, "output"))
            if nxt != chacha_block(key, 0, nonce)[:32]:
                bad.append((i, "next key"))
            if nxt == key:
                bad.append((i, "the key stayed"))
        check(f"random_bytes() is ChaCha20 under the current key, and the "
              f"key is replaced by the block before it ({modelled} "
              f"requests modelled exactly, problems: {bad[:5]})",
              not bad and modelled >= 70)
        check(f"the pool is drawn on at the first request, every 64th and "
              f"after 64 interrupts ({pooled} times in {len(sizes)})",
              pooled == 3)
        check("no two requests began with the same 32 bytes",
              len(outputs) == sum(1 for n in sizes if n >= 32))
    finally:
        h.close()


# --- On ThreadOS ----------------------------------------------------------

def run(q, command, timeout):
    seen = len(q.serial())
    q.type_line(command)
    name = command.split()[1]
    deadline = time.time() + timeout
    while time.time() < deadline:
        if f"[{name}] done, exit code" in q.serial()[seen:]:
            break
        time.sleep(0.5)
    out = q.serial()[seen:]
    m = re.search(r"\[%s\] done, exit code (-?\d+)" % re.escape(name), out)
    lines = [l.strip("\r") for l in out.replace("\r", "").split("\n")]
    body = [l for l in lines[1:] if l and not l.startswith(
            (f"[{name}]", "[kernel]", "[ OK ]", "[ !! ]", PROMPT))]
    return body, int(m.group(1)) if m else None


def one_boot(work, tag, cpu_model):
    iso = os.path.join(work, f"{tag}.iso")
    disk = os.path.join(work, f"{tag}.img")
    with open(disk, "wb") as fh:
        fh.write(b"\0" * DISK_SIZE)
    check(f"{tag}: the disc with the kernel builds",
          make_iso64(os.path.join(work, f"iso-{tag}"), iso))
    q = Qemu(KERNEL, INITRD, cdrom=iso, disk=disk, sata=True, cpus=2,
             serial=os.path.join(work, f"{tag}.log"),
             qemu="qemu-system-x86_64", cpu_model=cpu_model)
    try:
        q.wait_serial(PROMPT, timeout=90)
        q.wait_serial("] Disk ", timeout=30)
        banner = next((l.strip("\r") for l in q.serial().split("\n")
                       if "Random numbers" in l), "")
        got, code = run(q, "run randtest rand.bin dev.bin", 300)
    finally:
        q.close()
    img = TfsImage(disk)
    return banner, got, code, img.read("/rand.bin"), img.read("/dev.bin")


def judge(tag, data, size, where="the file is on the disk"):
    n = len(data)
    check(f"{tag}: {where}, {size} bytes", n == size)
    if n != size:
        return
    bits = n * 8
    ones = int.from_bytes(data, "little").bit_count()
    sigma = math.sqrt(bits) / 2
    check(f"{tag}: as many ones as zeros ({ones} of {bits}, "
          f"{(ones - bits / 2) / sigma:+.1f} sigma)",
          abs(ones - bits / 2) < 6 * sigma)
    counts = [0] * 256
    for b in data:
        counts[b] += 1
    e = n / 256
    chi = sum((c - e) ** 2 / e for c in counts)
    check(f"{tag}: every byte value about as often (chi-square {chi:.0f}, "
          "255 degrees of freedom)", abs(chi - 255) < 6 * math.sqrt(510))
    pairs = [0] * 65536
    for i in range(0, n, 2):
        pairs[data[i] << 8 | data[i + 1]] += 1
    e = n / 2 / 65536
    chi = sum((c - e) ** 2 / e for c in pairs)
    check(f"{tag}: every pair of bytes about as often (chi-square "
          f"{chi:.0f}, 65535 degrees of freedom)",
          abs(chi - 65535) < 6 * math.sqrt(2 * 65535))
    blocks = {data[i:i + 16] for i in range(0, n, 16)}
    check(f"{tag}: no 16-byte block comes twice ({n // 16 - len(blocks)} "
          "repeats)", len(blocks) == n // 16)
    packed = len(zlib.compress(data, 9))
    check(f"{tag}: zlib cannot shrink it ({packed} bytes from {n})",
          packed >= n)


def guest_checks(work):
    results = {}
    for tag, cpu, sources in (("default", None, "interrupt timing"),
                              ("max", "max", "RDSEED, RDRAND, interrupt "
                                             "timing")):
        banner, got, code, data, dev = one_boot(work, tag, cpu)
        check(f"{tag} processor: the kernel names its sources as "
              f"'{sources}' ({banner})",
              banner.endswith("sources: " + sources))
        for want in ("wrote 1048576 bytes to rand.bin",
                     "/dev/urandom: a character device, 262144 bytes "
                     "read into dev.bin",
                     "GRND_NONBLOCK once seeded: 16 bytes",
                     "getentropy of 256 bytes: ok",
                     "after fork: the parent's and the child's bytes differ",
                     "4 threads, 8000 draws of 16 bytes: all answered, "
                     "0 the same"):
            check(f"{tag}: randtest says '{want}'", want in got)
        check(f"{tag}: randtest ends with code 0 (got {code})", code == 0)
        sample = next((l.split()[1] for l in got if l.startswith("~sample ")),
                      None)
        results[tag] = (sample, data or b"")
        for what, d, size in (("getrandom", data, 1 << 20),
                              ("/dev/urandom", dev, 1 << 18)):
            if d is not None:
                judge(f"{tag}, {what}", d, size)
            else:
                check(f"{tag}: what {what} gave is on the disk", False)
    (s1, d1), (s2, d2) = results["default"], results["max"]
    shared = ({d1[i:i + 16] for i in range(0, len(d1), 16)} &
              {d2[i:i + 16] for i in range(0, len(d2), 16)})
    check("two boots share no 16-byte block, and their samples differ",
          s1 and s2 and s1 != s2 and d1 and d2 and not shared)


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    work = tempfile.mkdtemp()
    try:
        host_checks(work)
        guest_checks(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
