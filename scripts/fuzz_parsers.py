#!/usr/bin/env python3
"""Sprawdza parsery ProtonOS-a na uszkodzonych danych.

Dwa fronty: programy w ring 3 (certyfikaty, HTML, HTTP) i sciezka
pakietow w jadrze (net.c + tcp.c). W jadrze blad nie konczy sie smiercia
procesu, tylko padnieciem systemu, wiec tym bardziej warto go szukac.

Certyfikaty, HTML i odpowiedzi HTTP przychodza prosto z sieci - moze je
podac ktokolwiek. Ten skrypt kompiluje parsery na hoscie z sanitizerami
adresow i zachowan nieokreslonych, a potem karmi je tysiacami wejsc
zrobionych przez psucie poprawnych danych: obcinanie, przestawianie
bajtow, wpisywanie skrajnych dlugosci, wycinanie fragmentow.

Kazde wywalenie sie programu (albo zgloszenie sanitizera) to blad do
naprawienia - zapisujemy wtedy wejscie do pliku, zeby dalo sie je
powtorzyc.

Uzycie: python3 scripts/fuzz_parsers.py [liczba_przypadkow]
"""

import os
import random
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FUZZ = os.path.join(ROOT, "tests", "fuzz")
CASES = int(sys.argv[1]) if len(sys.argv) > 1 else 400


def build(workdir):
    """Kompiluje harness. Naglowki userspace wciagaja 'usys.h' po nazwie,
    a ta w wersji dla ProtonOS-a wola syscalle - podstawiamy wiec
    zaslepke, ustawiajac katalog z dowiazaniami do prawdziwych naglowkow
    i zaslepka obok nich."""
    inc = os.path.join(workdir, "include")
    os.makedirs(inc, exist_ok=True)

    for name in os.listdir(os.path.join(ROOT, "user")):
        if name.endswith(".h") and name != "usys.h":
            os.symlink(os.path.join(ROOT, "user", name),
                       os.path.join(inc, name))
    shutil.copy(os.path.join(FUZZ, "stub_usys.h"), os.path.join(inc, "usys.h"))

    binary = os.path.join(workdir, "harness")
    cmd = ["gcc", "-I", inc, "-fsanitize=address,undefined",
           "-fno-omit-frame-pointer", "-g", "-O1", "-Wall",
           "-o", binary, os.path.join(FUZZ, "harness.c")]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr[:4000])
        raise SystemExit("nie udalo sie zbudowac harnessu")
    return binary


def build_kernel(workdir):
    """Kompiluje net.c i tcp.c z zaslepkami sprzetu - zrodla jadra sa
    dowiazane w kstub/, obok katalogu z podstawionymi naglowkami."""
    kstub = os.path.join(FUZZ, "kstub")
    inc = os.path.join(kstub, "include")
    objs = []

    for src in ("net.c", "tcp.c"):
        obj = os.path.join(workdir, src.replace(".c", ".o"))
        cmd = ["gcc", "-I", inc, "-fsanitize=address,undefined",
               "-fno-omit-frame-pointer", "-g", "-O1", "-Wall",
               "-c", os.path.join(kstub, src), "-o", obj]
        out = subprocess.run(cmd, capture_output=True, text=True)
        if out.returncode != 0:
            print(out.stderr[:3000])
            raise SystemExit("nie udalo sie zbudowac %s" % src)
        objs.append(obj)

    binary = os.path.join(workdir, "kharness")
    cmd = ["gcc", "-I", inc, "-fsanitize=address,undefined", "-g", "-O1",
           "-Wall", "-o", binary,
           os.path.join(FUZZ, "kernel_harness.c")] + objs
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        print(out.stderr[:3000])
        raise SystemExit("nie udalo sie zbudowac harnessu jadra")
    return binary


def frames():
    """Poprawne ramki ethernetowe: ARP, ICMP, UDP i TCP."""
    mac_us = bytes([0x52, 0x54, 0x00, 0x12, 0x34, 0x56])
    mac_them = bytes([0x52, 0x55, 0x0a, 0x00, 0x02, 0x02])
    ip_us = bytes([10, 0, 2, 15])
    ip_them = bytes([10, 0, 2, 2])

    def eth(kind, payload):
        return mac_us + mac_them + kind + payload

    def ip(proto, payload):
        total = 20 + len(payload)
        hdr = (bytes([0x45, 0x00]) + total.to_bytes(2, "big") +
               b"\x00\x01\x00\x00\x40" + bytes([proto]) + b"\x00\x00" +
               ip_them + ip_us)
        return eth(b"\x08\x00", hdr + payload)

    out = []

    arp = (b"\x00\x01\x08\x00\x06\x04\x00\x01" + mac_them + ip_them +
           b"\x00" * 6 + ip_us)
    out.append(eth(b"\x08\x06", arp))

    icmp = b"\x08\x00\x00\x00\x12\x34\x00\x01" + b"proton" * 4
    out.append(ip(1, icmp))

    udp_payload = b"zapytanie testowe"
    udp = (b"\x30\x39" + (53).to_bytes(2, "big") +
           (8 + len(udp_payload)).to_bytes(2, "big") + b"\x00\x00" +
           udp_payload)
    out.append(ip(17, udp))

    tcp = (b"\x30\x39" + (5555).to_bytes(2, "big") +
           b"\x00\x00\x00\x01\x00\x00\x00\x00" +
           b"\x50\x02\xff\xff\x00\x00\x00\x00")
    out.append(ip(6, tcp))
    return out


def seeds(workdir):
    """Poprawne dane wyjsciowe: certyfikaty (RSA i dwie krzywe), podpis,
    strona HTML i odpowiedz HTTP."""
    def openssl(*args, **kw):
        subprocess.run(["openssl"] + list(args), check=True,
                       capture_output=True, timeout=120, **kw)

    j = lambda n: os.path.join(workdir, n)
    out = {"x509": [], "ecdsa": [], "html": [], "http": []}

    for tag, keyargs in [("rsa", ["-newkey", "rsa:2048"]),
                         ("p256", ["-newkey", "ec", "-pkeyopt",
                                   "ec_paramgen_curve:prime256v1"]),
                         ("p384", ["-newkey", "ec", "-pkeyopt",
                                   "ec_paramgen_curve:secp384r1", "-sha384"])]:
        openssl("req", "-x509", *keyargs, "-nodes", "-keyout", j(tag + ".key"),
                "-out", j(tag + ".pem"), "-days", "3650",
                "-subj", "/CN=protonos.test",
                "-addext", "subjectAltName=DNS:protonos.test,DNS:*.test")
        openssl("x509", "-in", j(tag + ".pem"), "-outform", "DER",
                "-out", j(tag + ".der"))
        out["x509"].append(open(j(tag + ".der"), "rb").read())

    with open(j("msg"), "wb") as f:
        f.write(b"ProtonOS")
    openssl("dgst", "-sha256", "-sign", j("p256.key"), "-out", j("p256.sig"),
            j("msg"))
    out["ecdsa"].append(open(j("p256.sig"), "rb").read())

    out["html"].append(
        b"<html><head><title>x</title><style>a{}</style></head><body>"
        b"<h1>Naglowek</h1><p>Tekst z encja &amp; i odnosnikiem "
        b'<a href="/dalej">tutaj</a>.</p><ul><li>raz</li><li>dwa</li></ul>'
        b"<script>var x=1;</script></body></html>")
    out["html"].append(b"<p>" + b"slowo " * 500 + b"</p>")

    out["http"].append(
        b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
        b"Transfer-Encoding: chunked\r\nLocation: /gdzies\r\n\r\n")
    return out


def mutate(data, rng):
    """Psucie danych: obciecie, zmiana bajtow, wstawki, skrajne dlugosci."""
    b = bytearray(data)
    how = rng.randrange(6)

    if how == 0 and len(b) > 1:                    # obciecie
        del b[rng.randrange(1, len(b)):]
    elif how == 1 and b:                           # pojedyncze bajty
        for _ in range(rng.randrange(1, 8)):
            b[rng.randrange(len(b))] = rng.randrange(256)
    elif how == 2 and b:                           # skrajne wartosci
        for _ in range(rng.randrange(1, 5)):
            b[rng.randrange(len(b))] = rng.choice([0x00, 0x01, 0x7f, 0x80,
                                                   0xfe, 0xff, 0x30, 0x82])
    elif how == 3 and len(b) > 4:                  # wyciecie fragmentu
        i = rng.randrange(len(b) - 2)
        del b[i:i + rng.randrange(1, min(64, len(b) - i))]
    elif how == 4:                                 # doklejka smieci
        b += bytes(rng.randrange(256) for _ in range(rng.randrange(1, 64)))
    else:                                          # powtorzenie fragmentu
        if len(b) > 8:
            i = rng.randrange(len(b) - 4)
            b[i:i] = b[i:i + rng.randrange(1, 32)]
    return bytes(b)


def main():
    rng = random.Random(20260826)
    workdir = tempfile.mkdtemp(prefix="protonos-fuzz-")
    failures = []

    try:
        binary = build(workdir)
        kbinary = build_kernel(workdir)
        corpus = seeds(workdir)
        corpus["ramki"] = frames()
        print("harness zbudowany, przypadkow na parser: %d" % CASES)

        for parser, base in corpus.items():
            crashes = 0
            for i in range(CASES):
                data = mutate(rng.choice(base), rng)
                path = os.path.join(workdir, "case.bin")
                with open(path, "wb") as f:
                    f.write(data)

                cmd = ([kbinary, path] if parser == "ramki"
                       else [binary, parser, path])
                run = subprocess.run(cmd, capture_output=True, timeout=30)
                if run.returncode != 0:
                    crashes += 1
                    keep = os.path.join(ROOT, "fuzz-%s-%03d.bin" % (parser, i))
                    with open(keep, "wb") as f:
                        f.write(data)
                    failures.append((parser, keep,
                                     run.stderr.decode(errors="replace")))
                    if crashes >= 3:
                        break
            print("  %-6s %4d przypadkow, bledow: %d" %
                  (parser, CASES, crashes))
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    if failures:
        print("\nZNALEZIONE BLEDY:")
        for parser, path, err in failures[:5]:
            print("--- %s (%s) ---" % (parser, path))
            print("\n".join(err.strip().splitlines()[:12]))
        return 1

    print("\nWszystkie parsery przetrwaly uszkodzone dane.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
