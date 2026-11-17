#!/usr/bin/env python3
"""ThreadOS test: the libraries WPE's port of WebKit needs.

libgcrypt with libgpg-error, libtasn1, libwebp, libxkbcommon, HarfBuzz
with its ICU part, and libepoxy - each built from its release into
BUILD/libs64. libc/tests/wpedeps.c asks each of them questions on
ThreadOS, written here into a file and fetched over HTTP, and every
answer is worked out here with something else:

  libgcrypt: SHA-1, SHA-2 and SHA3-256 of messages of every length
  around a block's edge, of a megabyte, and of 300 KB in eight threads
  at once (hashlib); HMAC (hmac); AES-CBC, -CTR across a counter's
  carry, -GCM with and without data (and one bit of a tag wrong
  refused), AES key wrap; PBKDF2 and HKDF, RFC 6070's and 5869's
  vectors among them; ECDSA on P-256, P-384 and P-521 - signatures by
  OpenSSL taken and altered ones refused, signatures made here and a
  key made here holding up in OpenSSL; ECDH, X25519 and Ed25519
  (RFC 7748's and 8032's vectors among them), RSA PKCS#1 v1.5 and PSS -
  all against the cryptography package, which is OpenSSL

  libtasn1: public keys - RSA, EC and Ed25519 - that OpenSSL wrote,
  taken apart into what OpenSSL says is in them; an EC key put
  together here byte for byte as OpenSSL writes it; a cut-off one and
  one with an impossible length refused

  libwebp: pictures libwebp wrote through Pillow - lossy, lossless,
  palettes, alpha - decoded to the same pixels as Pillow's libwebp,
  an animation's every frame and time, and a cut-off file refused

  libxkbcommon: a keymap from a string - Shift, Caps Lock with the
  types that take it and those that do not (where the Lock
  transformation applies), the third and fourth level, Control
  turning a letter into its control character - every key what the
  keymap's own definitions say; written out and read back the same;
  keysyms by name and their UTF-8 as the X11 tables give them; a
  broken keymap refused

  HarfBuzz: a font made here with fontTools, with its own ligatures,
  kerning, small capitals, a mark anchored above its base and Hebrew
  letters: shaped, every glyph, advance and offset is what the font's
  features say, right to left where the text is Hebrew - with
  HarfBuzz's own Unicode tables and with ICU's; and Unicode properties
  through ICU and HarfBuzz, against Python's unicodedata

  libepoxy: extension names found in a list as they are (and not as a
  prefix or part of another); and no EGL on ThreadOS - said, not a
  crash

Run on its own or through scripts/run_tests.py.
"""

import hashlib
import hmac
import io
import os
import random
import re
import shutil
import sys
import tempfile
import time
import unicodedata

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import ptest                 # noqa: E402
from ptest import *          # noqa: F401,F403,E402
from ptest import check, summary  # noqa: E402
import webpcases             # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def gen(n, seed):
    """wpedeps.c's gen(): bytes both sides make without sending them"""
    x = (seed * 2654435761 + 1) & 0xFFFFFFFF
    out = bytearray(n)
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        out[i] = (x >> 16) & 0xFF
    return bytes(out)


def hx(b):
    return b.hex() if b else "-"


class Questions:
    def __init__(self):
        self.lines = []          # (question, expected answer or a judge)
        self.files = {}          # name -> bytes, fetched before the run

    def ask(self, line, want):
        self.lines.append((line, want))


# --- libgcrypt and libtasn1, against OpenSSL ---------------------------------

def crypto(qs, rnd):
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import (
        ec, ed25519, padding, rsa, x25519)
    from cryptography.hazmat.primitives.asymmetric.utils import (
        decode_dss_signature, encode_dss_signature)
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    from cryptography.hazmat.primitives.kdf.hkdf import HKDF
    from cryptography.hazmat.primitives.keywrap import aes_key_wrap

    def b(n):
        return bytes(rnd.randrange(256) for _ in range(n))

    mk = open(os.path.join(ROOT, "Makefile")).read()
    gv = re.search(r"libgcrypt-([\d.]+)\.tar", mk).group(1)
    ev = re.search(r"libgpg-error-([\d.]+)\.tar", mk).group(1)
    qs.ask("version", "libgcrypt %s libgpg-error %s" % (gv, ev))

    py = {"sha1": "sha1", "sha224": "sha224", "sha256": "sha256",
          "sha384": "sha384", "sha512": "sha512", "sha3-256": "sha3_256",
          "md5": "md5"}
    edges = [0, 1, 3, 55, 56, 63, 64, 65, 111, 112, 127, 128, 129, 1000]
    for alg in py:
        for n in edges if alg in ("sha256", "sha512") else [0, 3, 64, 1000]:
            m = b(n)
            qs.ask("digest %s %s" % (alg, hx(m)),
                   hashlib.new(py[alg], m).hexdigest())
    for alg, n, seed in (("sha256", 1000000, 7), ("sha512", 1000003, 9),
                         ("sha1", 777777, 3), ("sha3-256", 300001, 5)):
        qs.ask("digestn %s %d %d" % (alg, n, seed),
               hashlib.new(py[alg], gen(n, seed)).hexdigest())
    for alg, n, seed, threads in (("sha256", 300000, 11, 4),
                                  ("sha512", 200000, 12, 8)):
        qs.ask("digestt %s %d %d %d" % (alg, n, seed, threads),
               "%s all %d threads alike: yes"
               % (hashlib.new(py[alg], gen(n, seed)).hexdigest(), threads))

    for alg in ("sha1", "sha256", "sha384", "sha512"):
        for kl in (1, 20, 64, 65, 131, 200):
            k, m = b(kl), b(rnd.randrange(0, 300))
            qs.ask("hmac %s %s %s" % (alg, hx(k), hx(m)),
                   hmac.new(k, m, alg).hexdigest())

    for kl in (16, 24, 32):
        for n in (16, 64, 160):
            k, iv, pt = b(kl), b(16), b(n)
            e = Cipher(algorithms.AES(k), modes.CBC(iv)).encryptor()
            qs.ask("aes-cbc %s %s %s" % (hx(k), hx(iv), hx(pt)),
                   (e.update(pt) + e.finalize()).hex() + " back: yes")
    for kl, ctr in ((16, None), (32, b"\xff" * 15 + b"\xfe"),
                    (24, b"\x00" * 8 + b"\xff" * 8)):
        for n in (1, 37, 100):
            k, c, pt = b(kl), ctr or b(16), b(n)
            e = Cipher(algorithms.AES(k), modes.CTR(c)).encryptor()
            qs.ask("aes-ctr %s %s %s" % (hx(k), hx(c), hx(pt)),
                   (e.update(pt) + e.finalize()).hex() + " back: yes")
    for kl, al, n in ((16, 0, 0), (16, 13, 16), (32, 32, 45), (24, 7, 100)):
        k, iv, aad, pt = b(kl), b(12), b(al), b(n)
        out = AESGCM(k).encrypt(iv, pt, aad or None)
        qs.ask("aes-gcm %s %s %s %s" % (hx(k), hx(iv), hx(aad), hx(pt)),
               "%s %s a wrong tag: refused" % (out[:-16].hex(), out[-16:].hex()))
    for kl, n in ((16, 16), (32, 32), (24, 24), (16, 40)):
        kek, key = b(kl), b(n)
        qs.ask("aes-kw %s %s" % (hx(kek), hx(key)), aes_key_wrap(kek, key).hex())

    # PBKDF2: RFC 6070's first two, then others of every length
    for alg, it, pw, salt, n in (
            ("sha1", 1, b"password", b"salt", 20),
            ("sha1", 4096, b"password", b"salt", 20),
            ("sha256", 1000, b(12), b(16), 32),
            ("sha512", 2000, b(8), b(24), 64),
            ("sha384", 10, b(30), b(8), 100)):
        qs.ask("pbkdf2 %s %d %s %s %d" % (alg, it, hx(pw), hx(salt), n),
               hashlib.pbkdf2_hmac(alg, pw, salt, it, n).hex())
    hk = {"sha1": hashes.SHA1(), "sha256": hashes.SHA256(),
          "sha512": hashes.SHA512()}
    for alg, ikm, salt, info, n in (
            ("sha256", b"\x0b" * 22, bytes(range(13)),
             bytes(range(0xf0, 0xfa)), 42),             # RFC 5869, case 1
            ("sha512", b(40), b(20), b(30), 100),
            ("sha1", b(11), b(11), b(5), 50)):
        qs.ask("hkdf %s %s %s %s %d" % (alg, hx(ikm), hx(salt), hx(info), n),
               HKDF(hk[alg], n, salt, info).derive(ikm).hex())

    curves = (("P-256", ec.SECP256R1(), "sha256", hashes.SHA256(), 32),
              ("P-384", ec.SECP384R1(), "sha384", hashes.SHA384(), 48),
              ("P-521", ec.SECP521R1(), "sha512", hashes.SHA512(), 66))
    order = {"P-256": 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551,
             "P-384": int("ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf"
                          "581a0db248b0a77aecec196accc52973", 16),
             "P-521": int("01ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
                          "fffa51868783bf2f966b7fcc0148f709a5d03bb5c9b8899c47aebb6fb71e9138"
                          "6409", 16)}

    def point(pub):
        return pub.public_bytes(serialization.Encoding.X962,
                                serialization.PublicFormat.UncompressedPoint)

    def num(v, w):
        return v.to_bytes(w, "big").hex()

    for name, curve, alg, h, w in curves:
        d = rnd.randrange(1, order[name])
        key = ec.derive_private_key(d, curve)
        q = point(key.public_key())
        m = b(rnd.randrange(1, 200))
        r, s = decode_dss_signature(key.sign(m, ec.ECDSA(h)))
        qs.ask("ecdsa-verify %s %s %s %s %s %s"
               % (name, hx(q), alg, hx(m), num(r, w), num(s, w)), "good")
        m2 = bytes([m[0] ^ 1]) + m[1:]
        qs.ask("ecdsa-verify %s %s %s %s %s %s"
               % (name, hx(q), alg, hx(m2), num(r, w), num(s, w)), "bad")
        qs.ask("ecdsa-verify %s %s %s %s %s %s"
               % (name, hx(q), alg, hx(m), num(r, w),
                  num((s + 1) % order[name], w)), "bad")

        def judge_sig(ans, pub=key.public_key(), m=m, h=h, w=w):
            parts = ans.split()
            if len(parts) != 2 or any(len(p) != 2 * w for p in parts):
                return False
            try:
                pub.verify(encode_dss_signature(int(parts[0], 16),
                                                int(parts[1], 16)),
                           m, ec.ECDSA(h))
                return True
            except Exception:
                return False
        judge_sig.__doc__ = "a signature OpenSSL verifies with the key"
        qs.ask("ecdsa-sign %s %s %s %s" % (name, num(d, w), alg, hx(m)),
               judge_sig)

        if name != "P-521":
            def judge_keygen(ans, curve=curve, m=m, h=h, w=w):
                parts = ans.split()
                if len(parts) != 3:
                    return False
                try:
                    pub = ec.EllipticCurvePublicKey.from_encoded_point(
                        curve, bytes.fromhex(parts[0]))
                    pub.verify(encode_dss_signature(int(parts[1], 16),
                                                    int(parts[2], 16)),
                               m, ec.ECDSA(h))
                    return True
                except Exception:
                    return False
            judge_keygen.__doc__ = ("a point on the curve, and a signature "
                                    "OpenSSL verifies with it")
            qs.ask("ecdsa-keygen %s %s %s" % (name, alg, hx(m)), judge_keygen)

        d2 = rnd.randrange(1, order[name])
        peer = ec.derive_private_key(d2, curve).public_key()
        qs.ask("ecdh %s %s %s" % (name, num(d, w), hx(point(peer))),
               key.exchange(ec.ECDH(), peer).hex())

    # X25519: RFC 7748 5.2's first vector, then others
    xs = [(bytes.fromhex("a546e36bf0527c9d3b16154b82465edd"
                         "62144c0ac1fc5a18506a2244ba449ac4"),
           bytes.fromhex("e6db6867583030db3594c1a424b15f7c"
                         "726624ec26b3353b10a903a6d0ab1c4c"))]
    for _ in range(3):
        xs.append((b(32), x25519.X25519PrivateKey.from_private_bytes(b(32))
                   .public_key().public_bytes_raw()))
    for k, u in xs:
        qs.ask("x25519 %s %s" % (hx(k), hx(u)),
               x25519.X25519PrivateKey.from_private_bytes(k).exchange(
                   x25519.X25519PublicKey.from_public_bytes(u)).hex())

    # Ed25519: RFC 8032 7.1's first vector, then others
    eds = [(bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc4"
                          "4449c5697b326919703bac031cae7f60"), b"")]
    eds += [(b(32), b(n)) for n in (1, 64, 300)]
    for seed, m in eds:
        k = ed25519.Ed25519PrivateKey.from_private_bytes(seed)
        sig = k.sign(m)
        qs.ask("ed25519-sign %s %s" % (hx(seed), hx(m)), sig.hex())
        pub = k.public_key().public_bytes_raw()
        qs.ask("ed25519-verify %s %s %s" % (hx(pub), hx(m), sig.hex()), "good")
        qs.ask("ed25519-verify %s %s %s" % (hx(pub), hx(m + b"!"), sig.hex()),
               "bad")

    rk = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    n_ = rk.public_key().public_numbers().n
    for scheme, alg, h, pad in (
            ("pkcs1", "sha256", hashes.SHA256(), padding.PKCS1v15()),
            ("pss", "sha256", hashes.SHA256(),
             padding.PSS(padding.MGF1(hashes.SHA256()), 32)),
            ("pss", "sha512", hashes.SHA512(),
             padding.PSS(padding.MGF1(hashes.SHA512()), 64))):
        m = b(rnd.randrange(1, 300))
        sig = rk.sign(m, pad, h)
        qs.ask("rsa-verify %s %s 010001 %s %s %s"
               % (scheme, num(n_, 256), alg, hx(m), sig.hex()), "good")
        qs.ask("rsa-verify %s %s 010001 %s %s %s"
               % (scheme, num(n_, 256), alg, hx(m + b"x"), sig.hex()), "bad")

    # libtasn1: what OpenSSL wrote, taken apart
    def spki(pub):
        return pub.public_bytes(serialization.Encoding.DER,
                                serialization.PublicFormat.SubjectPublicKeyInfo)

    def integer(v):
        """An INTEGER's content octets: two's complement, as short as can be"""
        return v.to_bytes(v.bit_length() // 8 + 1, "big").hex()
    der = spki(rk.public_key())
    qs.ask("spki %s" % der.hex(),
           "algorithm 1.2.840.113549.1.1.1 modulus %s exponent %s"
           % (integer(n_), integer(65537)))
    for name, curve, oid in (("P-256", ec.SECP256R1(), "1.2.840.10045.3.1.7"),
                             ("P-384", ec.SECP384R1(), "1.3.132.0.34")):
        pub = ec.derive_private_key(rnd.randrange(1, order[name]),
                                    curve).public_key()
        qs.ask("spki %s" % spki(pub).hex(),
               "algorithm 1.2.840.10045.2.1 curve %s point %s"
               % (oid, point(pub).hex()))
        qs.ask("spki-ec %s %s" % (oid, point(pub).hex()), spki(pub).hex())
    edpub = ed25519.Ed25519PrivateKey.from_private_bytes(b(32)).public_key()
    qs.ask("spki %s" % spki(edpub).hex(),
           "algorithm 1.3.101.112 point %s" % edpub.public_bytes_raw().hex())

    def refused(ans):
        return ans.startswith("refused (")
    refused.__doc__ = "refused, with libtasn1's reason"
    qs.ask("spki %s" % der[:-7].hex(), refused)
    qs.ask("spki %s" % (b"\x30\x84\x7f\xff\xff\xff" + der[4:40]).hex(), refused)


# --- libwebp, against Pillow's ---------------------------------------------

def webp(qs):
    from PIL import Image
    wanted = ("lossy 1x1 quality 50 method 4",
              "lossy 64x48 quality 50 method 4",
              "lossy 333x211 quality 90 method 6",
              "lossless 257x99 quality 100 method 6",
              "lossless, a palette of 16",
              "lossless, alpha exact",
              "lossy with alpha, alpha quality 30")
    cases = {name: data for name, data, _ in webpcases.cases()}
    for i, name in enumerate(wanted):
        data = cases[name]
        fn = "p%d.webp" % i
        qs.files[fn] = data
        im = Image.open(io.BytesIO(data))
        alpha = "yes" if im.mode == "RGBA" else "no"
        px = im.convert("RGBA")
        qs.ask("webp %s" % fn, "%dx%d alpha %s %s"
               % (px.width, px.height, alpha,
                  hashlib.sha256(px.tobytes()).hexdigest()))
    qs.files["cut.webp"] = cases[wanted[2]][:len(cases[wanted[2]]) // 2]
    qs.ask("webp cut.webp", "refused")

    # An animation of three frames, each its own length, played three times
    frames = [webpcases._picture(48, 32, 20 + k) for k in range(3)]
    f = io.BytesIO()
    frames[0].save(f, "WEBP", save_all=True, append_images=frames[1:],
                   duration=[100, 250, 80], loop=3, quality=80)
    qs.files["anim.webp"] = f.getvalue()
    im = Image.open(io.BytesIO(f.getvalue()))
    want = "%dx%d frames %d loop %d" % (im.width, im.height, im.n_frames,
                                        im.info.get("loop", 0))
    t = 0
    for k in range(im.n_frames):
        im.seek(k)
        im.load()                       # (the frame's times come with it)
        t += im.info["duration"]
        want += " %d:%s" % (t, hashlib.sha256(
            im.convert("RGBA").tobytes()).hexdigest())
    qs.ask("webp-anim anim.webp", want)


# --- libxkbcommon, against the keymap's own definitions ----------------------

KEYMAP = """xkb_keymap {
  xkb_keycodes "wpedeps" {
    minimum = 8;
    maximum = 255;
    <ESC> = 9; <AE01> = 10; <AE02> = 11; <AD01> = 24; <AD03> = 26;
    <LCTL> = 37; <AC01> = 38; <AC02> = 39; <LFSH> = 50; <AB01> = 52;
    <SPCE> = 65; <CAPS> = 66; <RALT> = 108;
  };
  xkb_types "wpedeps" {
    virtual_modifiers LevelThree;
    type "ONE_LEVEL" {
      modifiers = none;
      level_name[Level1] = "Any";
    };
    type "TWO_LEVEL" {
      modifiers = Shift;
      map[Shift] = Level2;
      level_name[Level1] = "Base";
      level_name[Level2] = "Shift";
    };
    type "ALPHABETIC" {
      modifiers = Shift + Lock;
      map[Shift] = Level2;
      map[Lock] = Level2;
      level_name[Level1] = "Base";
      level_name[Level2] = "Caps";
    };
    type "FOUR_LEVEL" {
      modifiers = Shift + LevelThree;
      map[Shift] = Level2;
      map[LevelThree] = Level3;
      map[Shift + LevelThree] = Level4;
      level_name[Level1] = "Base";
      level_name[Level2] = "Shift";
      level_name[Level3] = "AltGr";
      level_name[Level4] = "Shift AltGr";
    };
  };
  xkb_compatibility "wpedeps" {
    virtual_modifiers LevelThree;
    interpret Shift_L { action = SetMods(modifiers = Shift); };
    interpret Caps_Lock { action = LockMods(modifiers = Lock); };
    interpret Control_L { action = SetMods(modifiers = Control); };
    interpret ISO_Level3_Shift {
      virtualModifier = LevelThree;
      action = SetMods(modifiers = LevelThree);
    };
  };
  xkb_symbols "wpedeps" {
    name[Group1] = "WPE deps";
    key <ESC>  { [ Escape ] };
    key <AE01> { type = "TWO_LEVEL", [ 1, exclam ] };
    key <AE02> { type = "FOUR_LEVEL", [ 2, at, U2082, dead_acute ] };
    key <AD01> { type = "FOUR_LEVEL", [ q, Q, at, Greek_OMEGA ] };
    key <AD03> { type = "FOUR_LEVEL", [ e, E, EuroSign, cent ] };
    key <AC01> { type = "FOUR_LEVEL", [ a, A, aring, Aring ] };
    key <AC02> { type = "ALPHABETIC", [ s, S ] };
    key <AB01> { type = "ALPHABETIC", [ z, Z ] };
    key <SPCE> { [ space ] };
    key <LFSH> { [ Shift_L ] };
    key <CAPS> { [ Caps_Lock ] };
    key <LCTL> { [ Control_L ] };
    key <RALT> { [ ISO_Level3_Shift ] };
    modifier_map Shift { <LFSH> };
    modifier_map Lock { <CAPS> };
    modifier_map Control { <LCTL> };
    modifier_map Mod5 { <RALT> };
  };
};
"""

# What each tap gives, by the definitions above and XKB's rules: a
# type's map picks the level; Lock that a key's type does not take
# turns the keysym into its capital (the Lock transformation); Control
# turns a letter's character into its control character. The UTF-8 of
# a keysym is the X11 tables': aring is U+00E5, EuroSign U+20AC,
# Greek_OMEGA U+03A9, cent U+00A2, U2082 U+2082 - which the tables name
# twosubscript - and a dead key nothing.
def sym(name, ch):
    return "%s/%s" % (name, ch.encode("utf-8").hex())


XKB_TAPS = [
    ("AC01 AC02 AB01 SPCE AE01 ESC",
     [sym("a", "a"), sym("s", "s"), sym("z", "z"), sym("space", " "),
      sym("1", "1"), sym("Escape", "\x1b")]),
    ("+LFSH AC01 AC02 AE01 AE02 AD01 -LFSH AC01",
     [sym("A", "A"), sym("S", "S"), sym("exclam", "!"), sym("at", "@"),
      sym("Q", "Q"), sym("a", "a")]),
    # Caps Lock: ALPHABETIC takes Lock (level 2); FOUR_LEVEL does not,
    # and its letter is made a capital; a digit has none; Shift and Lock
    # together on ALPHABETIC are no entry of its map - level 1
    ("+CAPS -CAPS AC02 AB01 AC01 AE01 +LFSH AC02 -LFSH",
     [sym("S", "S"), sym("Z", "Z"), sym("A", "A"), sym("1", "1"),
      sym("s", "s")]),
    ("+RALT AC01 AD03 AE02 AD01 -RALT",
     [sym("aring", "\u00e5"), sym("EuroSign", "\u20ac"),
      sym("twosubscript", "\u2082"), sym("at", "@")]),
    ("+RALT +LFSH AC01 AD03 AE02 AD01 -LFSH -RALT",
     [sym("Aring", "\u00c5"), sym("cent", "\u00a2"), "dead_acute/",
      sym("Greek_OMEGA", "\u03a9")]),
    ("+LCTL AC01 AB01 AC02 -LCTL",
     [sym("a", "\x01"), sym("z", "\x1a"), sym("s", "\x13")]),
    ("+CAPS -CAPS +RALT AC01 -RALT", [sym("Aring", "\u00c5")]),
]

XKB_SYMS = [("a", 0x61, "a"), ("aring", 0xE5, "\u00e5"),
            ("EuroSign", 0x20AC, "\u20ac"), ("Greek_OMEGA", 0x7D9, "\u03a9"),
            ("U1F600", 0x101F600, "\U0001F600"), ("Return", 0xFF0D, "\r"),
            ("BackSpace", 0xFF08, "\b"), ("dead_acute", 0xFE51, ""),
            ("nosuchkeysym", 0, "")]


def xkb(qs):
    qs.files["keymap.xkb"] = KEYMAP.encode()
    qs.files["broken.xkb"] = KEYMAP.replace("type \"FOUR_LEVEL\" {",
                                            "type \"FOUR_LEVEL\" {{").encode()
    qs.ask("xkb-load broken.xkb", "refused")
    qs.ask("xkb-load keymap.xkb",
           "loaded, 1 layout, written out and read again: the same")
    for taps, want in XKB_TAPS:
        qs.ask("xkb " + taps, " ".join(want))
    qs.ask("xkb-sym " + " ".join(n for n, _, _ in XKB_SYMS),
           " ".join("%x/%s" % (v, c.encode("utf-8").hex())
                    for _, v, c in XKB_SYMS))


# --- HarfBuzz, against the font's own definitions ----------------------------

GLYPHS = [(".notdef", 500, None), ("space", 250, 0x20), ("A", 600, 0x41),
          ("V", 610, 0x56), ("T", 560, 0x54), ("o", 520, 0x6F),
          ("f", 300, 0x66), ("i", 230, 0x69), ("l", 240, 0x6C),
          ("f_i", 520, None), ("f_l", 530, None), ("a", 510, 0x61),
          ("a.sc", 470, None), ("acutecomb", 300, 0x301),
          ("alef", 640, 0x5D0), ("bet", 650, 0x5D1), ("gimel", 450, 0x5D2)]
KERN = {("A", "V"): -80, ("V", "A"): -70, ("T", "o"): -60}
BASE_ANCHOR, MARK_ANCHOR = (300, 700), (150, 500)
FEATURES = """
languagesystem DFLT dflt;
languagesystem latn dflt;
languagesystem hebr dflt;

feature liga {
    sub f i by f_i;
    sub f l by f_l;
} liga;

feature smcp {
    sub a by a.sc;
} smcp;

feature kern {
%s
} kern;

markClass acutecomb <anchor %d %d> @TOP;

feature mark {
    pos base [A o a] <anchor %d %d> mark @TOP;
} mark;

table GDEF {
    GlyphClassDef [space A V T o f i l a a.sc alef bet gimel], [f_i f_l],
                  [acutecomb], ;
} GDEF;
""" % ("\n".join("    pos %s %s %d;" % (a, b, v) for (a, b), v in KERN.items()),
       MARK_ANCHOR[0], MARK_ANCHOR[1], BASE_ANCHOR[0], BASE_ANCHOR[1])


def make_font():
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
    names = [g[0] for g in GLYPHS]
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({cp: n for n, _, cp in GLYPHS if cp})
    glyphs = {}
    for n in names:
        pen = TTGlyphPen(None)
        if n != "space":
            pen.moveTo((40, 0))
            pen.lineTo((40, 500))
            pen.lineTo((200, 500))
            pen.lineTo((200, 0))
            pen.closePath()
        glyphs[n] = pen.glyph()
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({n: (adv, 40) for n, adv, _ in GLYPHS})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": "WPE deps test", "styleName": "Regular"})
    fb.setupOS2()
    fb.setupPost()
    addOpenTypeFeaturesFromString(fb.font, FEATURES)
    out = io.BytesIO()
    fb.save(out)
    return out.getvalue()


def shaped(text, features=()):
    """What the font's features make of the text, as wpedeps prints it:
    the direction and script, then each glyph - id@cluster+advances
    <offsets> - in visual order"""
    gid = {n: k for k, (n, _, _) in enumerate(GLYPHS)}
    adv = {n: a for n, a, _ in GLYPHS}
    cmap = {cp: n for n, _, cp in GLYPHS if cp}
    run = [[cmap[ord(c)], i, 0, 0] for i, c in enumerate(text)]
    if "-liga" not in features:
        out, k = [], 0
        while k < len(run):
            if run[k][0] == "f" and k + 1 < len(run) and \
                    run[k + 1][0] in ("i", "l"):
                out.append(["f_" + run[k + 1][0], run[k][1], 0, 0])
                k += 2
            else:
                out.append(run[k])
                k += 1
        run = out
    if "+smcp" in features:
        for g in run:
            if g[0] == "a":
                g[0] = "a.sc"
    for g in run:
        g.append(adv[g[0]])
    if "-kern" not in features:
        for a, b in zip(run, run[1:]):
            a[4] += KERN.get((a[0], b[0]), 0)
    for k, g in enumerate(run):
        if g[0] == "acutecomb":             # a mark: on its base, no advance
            base = run[k - 1]
            g[1] = base[1]
            g[2] = BASE_ANCHOR[0] - MARK_ANCHOR[0] - base[4]
            g[3] = BASE_ANCHOR[1] - MARK_ANCHOR[1]
            g[4] = 0
    rtl = any(0x590 <= ord(c) < 0x600 for c in text)
    if rtl:
        run.reverse()
    head = "rtl Hebr" if rtl else "ltr Latn"
    return head + "".join(" %d@%d+%d,0<%d,%d>" % (gid[n], cl, a, x, y)
                          for n, cl, x, y, a in run)


HB_TEXTS = [("AVA To", ()), ("AVA To", ("-kern",)), ("fil", ()),
            ("fil", ("-liga",)), ("flafi", ()), ("a", ()), ("a", ("+smcp",)),
            ("A\u0301", ()), ("o\u0301", ()), ("\u05d0\u05d1\u05d2", ())]
HB_PROPS = [0x41, 0x61, 0x30, 0x20, 0xA0, 0x301, 0x903, 0x20DD, 0x660,
            0x2160, 0xBD, 0x5F, 0x2D, 0x28, 0x29, 0xAB, 0xBB, 0x21, 0x24,
            0x5E, 0x2B, 0xA9, 0x2028, 0x2029, 0x1C5, 0x2B0, 0x5D0, 0xE000,
            0x0, 0x200D, 0x378, 0xD800, 0x316, 0x5B0, 0x93C, 0x94D, 0xE38]


def harfbuzz(qs):
    qs.files["shape.ttf"] = make_font()
    for funcs in ("ucd", "icu"):
        for text, feats in HB_TEXTS:
            qs.ask("hb shape.ttf %s %s %s"
                   % (funcs, ",".join(feats) or "default",
                      ",".join("%x" % ord(c) for c in text)),
                   shaped(text, feats))

    def props(cp):
        c = chr(cp)
        p = "%s/%d" % (unicodedata.category(c), unicodedata.combining(c))
        return p + " " + p
    qs.ask("hb-props " + " ".join("%x" % cp for cp in HB_PROPS),
           " ".join(props(cp) for cp in HB_PROPS))


# --- libepoxy ------------------------------------------------------------

def epoxy(qs):
    # A name is in the list when it stands there whole - not as the start
    # of a longer one. (epoxy looks for what follows a match only, so the
    # end of a longer name would count too; no such name is asked, as
    # every real one begins with GL_, EGL_ or GLX_.)
    lst = "GL_ARB_foo GL_ARB_foobar EGL_KHR_image_base GL_EXT_x"
    for name in ("GL_ARB_foo", "GL_ARB_fo", "GL_ARB_foob", "GL_ARB_foobar",
                 "EGL_KHR_image", "EGL_KHR_image_base", "GL_EXT_x"):
        qs.ask("epoxy-ext %s %s" % (lst.replace(" ", ","), name),
               "yes" if name in lst.split() else "no")
    qs.ask("epoxy-egl", "EGL here: no")


def questions():
    qs = Questions()
    rnd = random.Random(2026)
    crypto(qs, rnd)
    webp(qs)
    xkb(qs)
    harfbuzz(qs)
    epoxy(qs)
    qs.files["questions.txt"] = "".join(l + "\n" for l, _ in qs.lines).encode()
    return qs


def answers(text):
    """The '@N answer' lines, by N - the answer as printed, a leading space
    of an empty first field kept"""
    out = {}
    for line in text.replace("\r", "").split("\n"):
        m = re.match(r"^@(\d+) (.*)$", line)
        if m:
            out[int(m.group(1))] = m.group(2).rstrip()
    return out


def judge(qs, got):
    """Every question against its answer, a check per library"""
    groups = {}
    for k, (line, want) in enumerate(qs.lines, 1):
        word = line.split()[0]
        lib = ("libgcrypt" if not word.startswith(("spki", "webp", "xkb", "hb",
                                                  "epoxy")) else
               "libtasn1" if word.startswith("spki") else
               "libwebp" if word.startswith("webp") else
               "libxkbcommon" if word.startswith("xkb") else
               "HarfBuzz" if word.startswith("hb") else "libepoxy")
        ans = got.get(k)
        ok = ans is not None and (want(ans) if callable(want)
                                  else ans == want.rstrip())
        g = groups.setdefault(lib, [0, 0, []])
        g[1] += 1
        if ok:
            g[0] += 1
        elif len(g[2]) < 3:
            g[2].append("%s -> %r, wanted %s" % (
                line[:70], (ans or "")[:120],
                want.__doc__ if callable(want) else repr(want[:120])))
    for lib, (good, total, bad) in groups.items():
        check("%s: %d of %d answers as the other implementation has them%s"
              % (lib, good, total, "" if not bad else
                 " - " + "; ".join(bad)), good == total and total > 0)


def main():
    print("--- the libraries WPE's WebKit needs ---")
    try:
        from PIL import features
        have_pil = bool(features.check("webp"))
    except ImportError:
        have_pil = False
    try:
        import cryptography     # noqa: F401
        import fontTools        # noqa: F401
        have = True
    except ImportError:
        have = False
    check("Pillow with WebP, the cryptography package and fontTools are "
          "here to compare with", have_pil and have)
    if not (have_pil and have):
        return summary()

    qs = questions()
    for name, data in qs.files.items():
        ptest.SERVED["/" + name] = data
    work = tempfile.mkdtemp()
    disk = os.path.join(work, "disk.img")
    with open(disk, "wb") as fh:
        fh.truncate(32 * 1024 * 1024)
    port, stop_http = start_http_server()
    try:
        q = Qemu(KERNEL, INITRD, disk, append=TEXT_MODE,
                 serial=os.path.join(work, "s.log"), ram_mib=512)
        try:
            q.wait_serial("thread64>", timeout=90)
            q.wait_serial("DHCP: granted", timeout=60)
            fetched = 0
            for name in qs.files:
                out = q.command("run httpget 10.0.2.2 %d /%s %s"
                                % (port, name, name), "exit code", timeout=90)
                fetched += "exit code 0" in out
            check("the questions and the files reach the disk (%d of %d)"
                  % (fetched, len(qs.files)), fetched == len(qs.files))
            start = len(q.serial())
            t0 = time.time()
            out = q.command("run wpedeps questions.txt",
                            "[wpedeps] done, exit code", timeout=900)
            took = time.time() - t0
            out = q.serial()[start:]
            got = answers(out)
            check("wpedeps answers all %d questions (%d, in %.0f s)"
                  % (len(qs.lines), len(got), took),
                  len(got) == len(qs.lines) and
                  "wpedeps: %d questions answered" % len(qs.lines) in out)
            judge(qs, got)
            check("and ends with code 0",
                  "[wpedeps] done, exit code 0" in out)
        finally:
            q.close()
    finally:
        stop_http()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
