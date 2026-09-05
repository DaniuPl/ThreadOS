#!/usr/bin/env python3
"""ProtonOS smoke test: AES, and the two modes that seal a packet.

Until now the whole cryptography of the system stood on ChaCha20.
AES is the entry ticket to two places ChaCha cannot reach: the TLS
cipher suite that some servers offer alone, and WPA2 - which encrypts
a wireless frame with AES-CCM and nothing else.

A cipher is the easiest thing in the world to get almost right: an AES
with a mistake in the key schedule encrypts, decrypts, agrees with
itself perfectly and agrees with nobody else in the world. So it is
checked from outside, three ways:

  1. the published vectors - FIPS-197 for the block, SP 800-38A for the
     counter, the GCM specification and RFC 3610 for the two
     authenticated modes;
  2. against OpenSSL - the system encrypts material of its own and
     prints everything it used, the host encrypts the same with
     libcrypto and compares byte for byte;
  3. in the other direction - OpenSSL encrypts something, the file
     travels into the system over HTTP, and the system has to decrypt
     it and to REJECT the same file with one bit changed.

The third one matters most: our encryption agreeing with our
decryption proves nothing about either.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
import ptest
from ptest import check, summary, no64, make_iso64, aesref, start_http_server


def fields(out):
    """The hex lines the program printed: klucz=..., szyfr=... ."""
    got = {}
    # The console ends its lines with \n\r, so the next line begins with
    # the carriage return - hence the \s* in front of the name.
    for name, value in re.findall(r"^\s*(klucz|nonce|aad|jawny|szyfr|tag)="
                                  r"([0-9a-f]+)\s*$", out, re.M):
        got[name] = bytes.fromhex(value)
    return got


def vector_file(mode, key, nonce, aad, plain, taglen):
    """The format user/aestest.c reads: a header, then the material."""
    ref = aesref(mode, key, nonce, aad, plain, taglen)
    if ref is None:
        return None
    ct, tag = ref
    head = struct.pack("<4sBBBBBBH", b"AESV", 1 if mode == "gcm" else 2,
                       len(key), len(nonce), len(aad), taglen, 0, len(ct))
    return head + key + nonce + aad + ct + tag


def main():
    print()
    print("--- AES, GCM i CCM ---")

    reason = no64()
    if reason:
        print("POMINIETE: " + reason)
        return summary()

    d = tempfile.mkdtemp()
    iso = tempfile.mktemp(suffix=".iso")
    slog = tempfile.mktemp(suffix=".log")
    disk = tempfile.mktemp(suffix=".img")
    with open(disk, "wb") as fh:
        fh.truncate(DISK_SIZE)

    # What OpenSSL is to encrypt for us, prepared before the server
    # starts - the guest downloads it as an ordinary file.
    key = bytes(range(0x40, 0x50))
    nonce12 = bytes(range(0x10, 0x1C))
    nonce13 = bytes(range(0x20, 0x2D))
    aad = b"ProtonOS, naglowek ramki"
    plain = bytes((i * 7 + 3) & 0xFF for i in range(48))

    good_gcm = vector_file("gcm", key, nonce12, aad, plain, 16)
    good_ccm = vector_file("ccm", key, nonce13, aad, plain, 8)
    have_ref = good_gcm is not None

    if have_ref:
        bad_gcm = bytearray(good_gcm)
        bad_gcm[-1] ^= 0x01          # one bit of the tag
        ptest.SERVED["/gcm.bin"] = good_gcm
        ptest.SERVED["/ccm.bin"] = good_ccm
        ptest.SERVED["/zly.bin"] = bytes(bad_gcm)

    try:
        check("plyta z jadrem 64-bitowym buduje sie",
              make_iso64(d, iso, text=True))
        if not os.path.exists(iso):
            return summary()

        port, stop = start_http_server()
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                 disk=disk, sata=True, qemu="qemu-system-x86_64")
        try:
            q.wait_serial("proton64>", timeout=90)

            # --- 1. The published vectors ------------------------------
            out = q.command("run aestest", "kod wyjscia", timeout=120)
            check("wektory z FIPS-197, SP 800-38A, GCM i RFC 3610",
                  "[aes] wszystko sie zgadza" in out and "BLAD" not in out)
            check("CCM odrzuca podmieniony znacznik i nie oddaje tresci",
                  "CCM wykrycie podmiany znacznika: OK" in out and
                  "CCM po odrzuceniu nie zostawia tresci: OK" in out)
            check("GCM wykrywa zmiane w danych dodatkowych",
                  "GCM wykrycie zmiany w naglowku: OK" in out)

            # --- 2. The same material through OpenSSL ------------------
            if not have_ref:
                print("POMINIETE: brak cc/libcrypto - bez porownania "
                      "z OpenSSL")
            else:
                cases = [("ecb", "3f", 32, 128), ("ecb", "77", 32, 256),
                         ("ctr", "5a", 40, 128), ("ctr", "b1", 40, 192),
                         ("gcm", "2c", 45, 128), ("gcm", "e9", 45, 256),
                         ("ccm", "84", 45, 128), ("ccm", "17", 45, 256)]
                for mode, seed, length, bits in cases:
                    out = q.command("run aestest %s %s %d %d"
                                    % (mode, seed, length, bits),
                                    "kod wyjscia", timeout=120)
                    got = fields(out)
                    if "szyfr" not in got or "klucz" not in got:
                        check("%s-%d: program wypisal material" % (mode, bits),
                              False)
                        continue

                    # A degenerate input would make the comparison
                    # meaningless - a key of one repeated byte agrees
                    # with everything.
                    check("%s-%d: material nie jest wyrodny" % (mode, bits),
                          len(set(got["klucz"])) > 4 and
                          len(set(got["jawny"])) > 4 and
                          len(got["klucz"]) == bits // 8)

                    taglen = 8 if mode == "ccm" else 16
                    ref = aesref(mode, got["klucz"],
                                 got.get("nonce", b""), got.get("aad", b""),
                                 got["jawny"], taglen)
                    check("%s-%d: szyfr taki sam jak w OpenSSL"
                          % (mode, bits),
                          ref is not None and ref[0] == got["szyfr"])
                    if mode in ("gcm", "ccm"):
                        check("%s-%d: znacznik taki sam jak w OpenSSL"
                              % (mode, bits),
                              ref is not None and ref[1] == got.get("tag"))
                    check("%s-%d: odszyfrowanie wraca do jawnego"
                          % (mode, bits), "powrot: OK" in out)

                # --- 3. A ciphertext made outside the system -----------
                q.wait_serial("DHCP: przyznano", timeout=60)
                for name, label in (("gcm", "GCM"), ("ccm", "CCM")):
                    q.command("run httpget 10.0.2.2 %d /%s.bin %s.bin"
                              % (port, name, name), "kod wyjscia",
                              timeout=60)
                    out = q.command("run aestest plik %s.bin" % name,
                                    "kod wyjscia", timeout=60)
                    check("%s: szyfr z OpenSSL odszyfrowany w systemie"
                          % label,
                          "[aes] tag zgadza sie" in out and
                          fields(out).get("jawny") == plain)

                q.command("run httpget 10.0.2.2 %d /zly.bin zly.bin" % port,
                          "kod wyjscia", timeout=60)
                out = q.command("run aestest plik zly.bin", "kod wyjscia",
                                timeout=60)
                check("jeden zmieniony bit znacznika = odmowa",
                      "[aes] tag NIE zgadza sie" in out and
                      "jawny=" not in out)
        finally:
            q.close()
            stop()

        # --- The same code compiled for the other mode -----------------
        # The 32-bit kernel is no longer developed, but it is still the
        # cheapest way to catch what one compilation hides: a shift, a
        # width, a pointer that happened to be the right size.
        disk32 = tempfile.mktemp(suffix=".img")
        with open(disk32, "wb") as fh:
            fh.truncate(DISK_SIZE)
        q = Qemu(KERNEL, INITRD, disk32, append=TEXT_MODE,
                 serial=tempfile.mktemp(suffix=".log"))
        try:
            q.wait_for("ProtonOS shell")
            q.type_line("run aestest")
            screen = q.wait_for("[aes] wszystko sie zgadza", timeout=120)
            check("te same wektory przechodza w trybie 32-bitowym",
                  "[aes] wszystko sie zgadza" in screen)
        finally:
            q.close()
            os.unlink(disk32)
    finally:
        for path in (iso, slog, disk):
            if os.path.exists(path):
                os.unlink(path)

    return summary()


if __name__ == "__main__":
    sys.exit(main())
