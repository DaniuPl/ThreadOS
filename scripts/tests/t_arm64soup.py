#!/usr/bin/env python3
"""ThreadOS smoke test: libsoup, glib-networking and OpenSSL on ARM64.

How Weft's network process fetches pages, on the ARM64 kernel: souptest
(libc/tests/souptest.c, the PC's) built for aarch64 by the PC's rules
(make arm64-libs), its CA in the initrd, a virtio network card - held to
exactly what t_soup.py holds the PC's to, against the same servers on
the host:

  HTTP/1.1: a page, a megabyte in chunks, gzip decoded, a redirect, a
  cookie sent back, a POST, a 404, four requests at once on the main
  loop - every body as its length and SHA-256, worked out here

  HTTPS through glib-networking's OpenSSL backend: TLS 1.3 to a server
  the CA signed, its subject and issuer; refused without the CA, and
  refused for a certificate that names another address

  HTTP/2 over TLS (nghttp2) when this machine has the h2 package; the
  public suffix list; OpenSSL's SHA-256 and HMAC
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ptest import *          # noqa: F401,F403
from ptest import check, summary, noarm64, arm64_boot, KERNEL
import t_soup

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SOUPTEST = os.path.join(os.path.dirname(os.path.abspath(KERNEL)), "aarch64",
                        "user64", "souptest.elf")
HOST = "10.0.2.2"            # the host, as QEMU's user network shows it


def main():
    print("--- libsoup, glib-networking, OpenSSL on the ARM64 kernel ---")
    why = noarm64()
    if why:
        print("SKIPPED: " + why)
        return summary()
    if not os.path.exists(SOUPTEST):
        print("SKIPPED: no aarch64 souptest (make arm64-libs)")
        return summary()
    d = tempfile.mkdtemp()
    servers = []
    try:
        t_soup.make_pki(d)
        plain = t_soup.serve(t_soup.Handler)
        good = t_soup.serve(t_soup.Handler, t_soup.tls_context(d, "good"))
        wrong = t_soup.serve(t_soup.Handler, t_soup.tls_context(d, "wrong"))
        servers += [plain, good, wrong]
        h2 = t_soup.h2_server(t_soup.tls_context(d, "good", ["h2"]))
        if h2 is None:
            print("NOTE: no h2 package here for an HTTP/2 server "
                  "(pip install h2) - HTTP/2 goes unchecked")
        want = t_soup.expected(d, t_soup.public_suffix_rules(),
                               h2 is not None)

        img = os.path.join(d, "initrd.img")
        subprocess.run([sys.executable, os.path.join(ROOT, "scripts",
                                                     "mkinitrd.py"), img,
                        "souptest=" + SOUPTEST,
                        "ca.pem=" + os.path.join(d, "ca.pem")],
                       check=True, capture_output=True)
        args = [f"http://{HOST}:{plain.server_address[1]}",
                f"https://{HOST}:{good.server_address[1]}", "ca.pem",
                f"https://{HOST}:{wrong.server_address[1]}"]
        if h2 is not None:
            args.append(f"https://{HOST}:{h2.getsockname()[1]}")
        said = arm64_boot(cores=2, mib=1024, initrd=img, timeout=600,
                          append="run=" + ",".join(["souptest"] + args),
                          extra=["-global", "virtio-mmio.force-legacy=false",
                                 "-netdev", "user,id=n0", "-device",
                                 "virtio-net-device,netdev=n0"])
        start = re.search(r"^\[proc\] souptest started, pid \d+$", said, re.M)
        end = re.search(r"^\[proc\] souptest \(pid \d+\) ended with code "
                        r"(-?\d+)$", said, re.M)
        body = said[start.end():end.start()] if start and end else ""
        got = [l.strip() for l in body.split("\n")
               if l.strip() and not l.startswith("[")]
        plain_lines = [l for l in got if not l.startswith("~")]
        first = next((i for i, (w, g) in enumerate(zip(want, plain_lines))
                      if w != g), None)
        where = (f"first difference at line {first + 1}: want "
                 f"{want[first]!r}, souptest {plain_lines[first]!r}"
                 if first is not None else
                 f"want {len(want)} lines, souptest {len(plain_lines)}")
        check(f"souptest on ARM64 says what the servers sent and what "
              f"Python works out ({where})", plain_lines == want)
        code = int(end.group(1)) if end else None
        check(f"souptest ends with code 0 (got {code})", code == 0)
        for l in got:
            if l.startswith("~"):
                print("   " + l)
    finally:
        for s in servers:
            s.shutdown()
        shutil.rmtree(d, ignore_errors=True)
    return summary()


if __name__ == "__main__":
    sys.exit(main())
