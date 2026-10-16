#!/usr/bin/env python3
"""ThreadOS smoke test: AES, and the two modes that seal a packet.

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
    """The hex lines the program printed: key=..., ciphertext=... ."""
    got = {}
    # The console ends its lines with \n\r, so the next line begins with
    # the carriage return - hence the \s* in front of the name.
    for name, value in re.findall(r"^\s*(key|nonce|aad|plaintext|ciphertext|tag)="
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
        print("SKIPPED: " + reason)
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
    aad = b"ThreadOS, a frame header"
    plain = bytes((i * 7 + 3) & 0xFF for i in range(48))

    good_gcm = vector_file("gcm", key, nonce12, aad, plain, 16)
    good_ccm = vector_file("ccm", key, nonce13, aad, plain, 8)
    have_ref = good_gcm is not None

    if have_ref:
        bad_gcm = bytearray(good_gcm)
        bad_gcm[-1] ^= 0x01          # one bit of the tag
        ptest.SERVED["/gcm.bin"] = good_gcm
        ptest.SERVED["/ccm.bin"] = good_ccm
        ptest.SERVED["/bad.bin"] = bytes(bad_gcm)

    try:
        check("the disc with the 64-bit kernel builds",
              make_iso64(d, iso, text=True))
        if not os.path.exists(iso):
            return summary()

        port, stop = start_http_server()
        q = Qemu(KERNEL, INITRD, cdrom=iso, serial=slog,
                 disk=disk, sata=True, qemu="qemu-system-x86_64")
        try:
            q.wait_serial("thread64>", timeout=90)

            # --- 1. The published vectors ------------------------------
            out = q.command("run aestest", "exit code", timeout=120)
            check("vectors from FIPS-197, SP 800-38A, GCM and RFC 3610",
                  "[aes] everything matches" in out and "FAILED" not in out)
            check("CCM refuses a changed tag and gives back no plaintext",
                  "CCM notices a changed tag: OK" in out and
                  "CCM leaves no plaintext after a refusal: OK" in out)
            check("GCM notices a change in the additional data",
                  "GCM notices a change in the header: OK" in out)

            # --- 2. The same material through OpenSSL ------------------
            if not have_ref:
                print("SKIPPED: no cc/libcrypto - without a comparison "
                      "z OpenSSL")
            else:
                cases = [("ecb", "3f", 32, 128), ("ecb", "77", 32, 256),
                         ("ctr", "5a", 40, 128), ("ctr", "b1", 40, 192),
                         ("gcm", "2c", 45, 128), ("gcm", "e9", 45, 256),
                         ("ccm", "84", 45, 128), ("ccm", "17", 45, 256),
                         ("ccm7", "6d", 45, 128)]
                for mode, seed, length, bits in cases:
                    out = q.command("run aestest %s %s %d %d"
                                    % (mode, seed, length, bits),
                                    "exit code", timeout=120)
                    got = fields(out)
                    if "ciphertext" not in got or "key" not in got:
                        check("%s-%d: the program printed the material" % (mode, bits),
                              False)
                        continue

                    # A degenerate input would make the comparison
                    # meaningless - a key of one repeated byte agrees
                    # with everything.
                    check("%s-%d: the material is not degenerate" % (mode, bits),
                          len(set(got["key"])) > 4 and
                          len(set(got["plaintext"])) > 4 and
                          len(got["key"]) == bits // 8)

                    # ccm7 is the same mode with a seven-byte nonce -
                    # OpenSSL takes the length from the nonce itself.
                    taglen = 8 if mode == "ccm" else 16
                    ref = aesref(mode[:3], got["key"],
                                 got.get("nonce", b""), got.get("aad", b""),
                                 got["plaintext"], taglen)
                    check("%s-%d: the ciphertext is the same as OpenSSL's"
                          % (mode, bits),
                          ref is not None and ref[0] == got["ciphertext"])
                    if mode in ("gcm", "ccm"):
                        check("%s-%d: the tag is the same as OpenSSL's"
                              % (mode, bits),
                              ref is not None and ref[1] == got.get("tag"))
                    check("%s-%d: decryption comes back to the plaintext"
                          % (mode, bits), "back again: OK" in out)

                # --- 3. A ciphertext made outside the system -----------
                q.wait_serial("DHCP: granted", timeout=60)
                for name, label in (("gcm", "GCM"), ("ccm", "CCM")):
                    q.command("run httpget 10.0.2.2 %d /%s.bin %s.bin"
                              % (port, name, name), "exit code",
                              timeout=60)
                    out = q.command("run aestest file %s.bin" % name,
                                    "exit code", timeout=60)
                    check("%s: OpenSSL's ciphertext decrypted inside the system"
                          % label,
                          "[aes] the tag matches" in out and
                          fields(out).get("plaintext") == plain)

                q.command("run httpget 10.0.2.2 %d /bad.bin bad.bin" % port,
                          "exit code", timeout=60)
                out = q.command("run aestest file bad.bin", "exit code",
                                timeout=60)
                check("one changed bit of the tag = a refusal",
                      "[aes] the tag does NOT match" in out and
                      "plaintext=" not in out)
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
            q.wait_for("ThreadOS shell")
            q.type_line("run aestest")
            screen = q.wait_for("[aes] everything matches", timeout=120)
            check("the same vectors pass in 32-bit mode",
                  "[aes] everything matches" in screen)
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
