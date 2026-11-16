#!/usr/bin/env python3
"""ThreadOS smoke test: the system's CA roots, trusted by both kinds of TLS.

ThreadOS has two TLS stacks: its own (user/utls.h), behind httpget and
so behind the browser, and Mbed TLS, behind curl and every musl
program. Both now trust the same roots: Mozilla's, as curl.se publishes
them (third_party/cacert/cacert.pem), carried in the initrd as
cacert.pem - httpget decodes it itself (cert_load_system in
user/ucert.h), curl reads it where Linux keeps it, made up by the C
library.

A root the test holds the key for cannot be one of Mozilla's, so the
test makes an initrd of its own: the build's, with the test's root
added to the end of cacert.pem. Against the test's server (a chain of
root, intermediate and server, from OpenSSL):

  with the build's initrd, both refuse it - httpget says there is no
  trusted issuer, curl ends with its code 60

  with the test's initrd, both accept it with nothing given to them -
  no roots.der, no --cacert - and fetch the page

and the build's initrd carries the bundle unchanged.

Run on its own or through scripts/run_tests.py.
"""

import os
import re
import shutil
import ssl
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ptest import *          # noqa: F401,F403
from ptest import (check, summary, make_iso64, no64, start_dns_server,
                   DNS_HINT, start_https_chain_server)
import ptest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROMPT = "thread64>"


def read_tird(path):
    """The files of an initrd (scripts/mkinitrd.py's format), in order"""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:4] != b"TIRD":
        raise ValueError("not an initrd")
    count, = struct.unpack_from("<I", data, 4)
    files = []
    for i in range(count):
        name, off, size = struct.unpack_from("<16sII", data, 8 + 24 * i)
        files.append((name.split(b"\0")[0].decode(), data[off:off + size]))
    return files


def write_tird(path, files):
    out = bytearray(b"TIRD") + struct.pack("<I", len(files))
    offset = 8 + 24 * len(files)
    for name, data in files:
        out += struct.pack("<16sII", name.encode(), offset, len(data))
        offset += len(data)
    for _, data in files:
        out += data
    with open(path, "wb") as fh:
        fh.write(out)


def fetch_both(work, tag, initrd, port):
    """httpget's and curl's answers from one boot with this initrd"""
    iso = os.path.join(work, tag + ".iso")
    check(f"{tag}: the disc builds",
          make_iso64(os.path.join(work, "iso-" + tag), iso, initrd=initrd))
    q = Qemu(KERNEL, initrd, cdrom=iso, serial=os.path.join(work, tag + ".log"),
             qemu="qemu-system-x86_64")
    try:
        q.wait_serial(PROMPT, timeout=90)
        q.wait_serial("[net] DHCP", timeout=60)
        q.command("dns 10.0.2.2", "DNS server set")
        hg = q.command(f"run httpget https://threados.test {port} /",
                       "[httpget] done, exit code", timeout=120)
        cu = q.command(f"run curl -s https://threados.test:{port}/",
                       "[curl] done, exit code", timeout=120)
    finally:
        q.close()
    m = re.search(r"\[curl\] done, exit code (-?\d+)", cu)
    return hg, cu, int(m.group(1)) if m else None


def main():
    reason = no64()
    if reason:
        print("SKIPPED: " + reason)
        return summary()
    dns_stop, dns_ok = start_dns_server(only="threados.test")
    if not dns_ok:
        print("SKIPPED: " + DNS_HINT)
        return summary()
    port, https_stop = start_https_chain_server()
    work = tempfile.mkdtemp()
    try:
        check("the test's chain is made (openssl)",
              port is not None and ptest.CERT_DER_ROOT)
        if port is None:
            return summary()
        with open(os.path.join(ROOT, "third_party", "cacert",
                               "cacert.pem"), "rb") as fh:
            mozilla = fh.read()
        files = read_tird(INITRD)
        carried = dict(files).get("cacert.pem")
        check("the initrd carries third_party/cacert/cacert.pem unchanged "
              f"({len(carried or b'')} bytes)", carried == mozilla)

        # The same initrd, the test's root at the end of the bundle
        root_pem = ssl.DER_cert_to_PEM_cert(ptest.CERT_DER_ROOT).encode()
        mine = os.path.join(work, "initrd-trust.img")
        write_tird(mine, [(n, d + root_pem if n == "cacert.pem" else d)
                          for n, d in files])

        hg, cu, code = fetch_both(work, "the build's initrd", INITRD, port)
        check("the build's initrd: httpget refuses the test's server - no "
              "trusted issuer", "no trusted issuer" in hg and
              "ThreadOS TCP works" not in hg)
        check(f"and so does curl, with code 60 (got {code})", code == 60)

        hg, cu, code = fetch_both(work, "the test's initrd", mine, port)
        check("the test's root in the system's bundle: httpget checks the "
              "chain and fetches the page, with no roots.der",
              "certificate checked" in hg and "ThreadOS TCP works" in hg)
        check(f"and curl does too, with no --cacert (code {code})",
              code == 0 and "ThreadOS TCP works" in cu)
    finally:
        https_stop()
        dns_stop()
        shutil.rmtree(work, ignore_errors=True)
    return summary()


sys.exit(main())
